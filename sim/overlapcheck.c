// The text overlap gate: does a screen actually draw the way it was laid out?
//
// Wallet screens position content at absolute y while the body font shrinks 28
// to 23 to 14 depending on how long the active locale's translation is (see
// wt_body_font). That pairing cannot be checked by reading the source. A card
// that fits in English grows quietly into the button beneath it in French, and
// the first person to find out is holding a device.
//
// sim/fitcheck.c cannot answer this and should not try. It measures strings
// from a static table with lv_text_get_size, never instantiates a screen, and
// its build script links only the theme, i18n and the fonts. So this links the
// whole wallet UI the way sim/sim_main.c does, and then reuses sim_main.c's
// scripted walk instead of growing a second copy that would drift from it.
// Every save() in that walk is a screen that has settled and been refreshed,
// which is precisely where a layout question belongs, so each one becomes a
// stop here and any frame added to the walk later is covered without anyone
// remembering to add it.
//
// Four questions at each stop, asked in screen coordinates after clipping:
//
//   1. TEXT     do two pieces of text share pixels
//   2. CONTENT  does something above the action row reach down into it
//   3. GROWTH   has a wrapping label grown into the text below it
//   4. CLIPPED  was text laid out and then cut away where nobody can reach it
//   5. ROLE     is one element wearing a themed accent and a status colour
//   6. BARE     is a screen just a wall of text, with none of the kit's chrome
//   7. WALL     is the only chrome a box drawn around that wall of text
//
// The first three are the ones the review asked for. The fourth was added after
// reading docs/media/sign-verify.png: a label can ask for a box taller than the
// container holding it, get clipped, and leave a row of glyphs sliced through
// the middle while the other three checks see a perfectly clean screen. Same
// root cause, absolute y under content that grows, so it belongs to this gate.
//
// The fifth is rule 1 of ADDENDUM-02 and it is here for the same reason the
// others are: it is a question about a rendered object, and this is the only
// gate that has one. It runs per accent rather than per locale, since colours
// do not change with language.
//
// Asked once per locale, because a gate that only speaks English measures the
// one language that was never going to break.
//
// A note on thresholds. Every tolerance in here was measured against renders
// rather than picked, and the reasoning is written down beside each one, because
// a gate tuned by feel is a gate that gets switched off the first week it cries
// wolf. The tuning pass took the English run from about 200 findings to the
// handful that survive, and each class removed is justified at its constant.
//
// Exit code is 0 unless OVERLAPCHECK_STRICT is set. That escape hatch existed
// so the gate could land in phase 0 and report honestly against layout nobody
// had fixed yet. Phase 1 fixed it and CI sets the variable, so the gate blocks.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "kiss_theme.h"
#include "colour_de.h"

// A hairline of shared area is not a bug. Label bounding boxes carry the full
// line height rather than the ink, so stacked text legitimately touches and
// occasionally laps a pixel. Two pixels in BOTH axes is the smallest overlap
// that means something is actually drawn on top of something else.
#define OC_SLOP 2

// A backdrop covering essentially the whole screen is a backdrop, not content.
// Used twice: such a thing is never itself a layout fault, and anything painted
// before it is hidden behind it. Measured as a fraction of the 800x480 panel
// rather than as a pixel count that would need editing if the panel changed.
#define OC_BACKDROP_PCT 90

#define OC_MAX_NODES 768
#define OC_MAX_SEEN  256

typedef struct {
    lv_obj_t *obj;
    lv_area_t vis;        // coords after every clipping ancestor has had its say
    lv_area_t nat;        // where it asked to be, before any of them
    bool cutx;            // ...and lost its left or right edge to it
    int       lh;         // line height of its font, 0 when it is not text
    bool      is_label;
    bool      wraps;
    bool      leaf;
    bool      clickable;
    bool      buried;     // an opaque backdrop is painted over the top of it
    bool      cut;        // its bottom was clipped away by something that does
                          // not scroll, so nobody can reach the rest of it
    lv_obj_t *parent;
} oc_node_t;

// Is this label more than one line tall? The distinction carries the whole
// signal to noise ratio of the TEXT check, see oc_vslop().
static bool oc_multiline(const oc_node_t *n)
{
    return n->lh > 0 && (n->nat.y2 - n->nat.y1 + 1) > n->lh * 3 / 2;
}

// How much shared area between two labels is normal rather than a fault.
//
// An LVGL label's box is a line box, not the ink. It carries the font's leading
// above and below the glyphs, so a caption sitting directly on top of the value
// it describes reports a few pixels of overlap while rendering perfectly. This
// was measured rather than guessed: on the sign screen "RECIPIENT GETS" over
// "60 000 sats" shares 9px and docs/media/sign-verify.png shows them cleanly
// stacked, while "CAUTION ..." over "I UNDERSTAND" shares 6px and is a genuine
// collision. Vertical extent alone cannot tell those apart.
//
// What tells them apart is growth. Every false positive was two SINGLE line
// labels stacked, and a single line label cannot grow when a translation runs
// long, which is the entire bug class this gate exists for. Every true positive
// involved a label that had wrapped to several lines. So a pair of single line
// labels is allowed the font's leading, and anything involving a grown label is
// held to the hairline.
static int oc_vslop(const oc_node_t *a, const oc_node_t *b)
{
    if (oc_multiline(a) || oc_multiline(b)) return OC_SLOP;
    int lh = a->lh < b->lh ? a->lh : b->lh;
    int slop = lh / 2;
    return slop > OC_SLOP ? slop : OC_SLOP;
}

static oc_node_t s_node[OC_MAX_NODES];
static int       s_n;

static int  s_stops;
static int  s_skipped;
static int  s_findings;
static char s_seen[OC_MAX_SEEN][192];
static int  s_seen_n;
static int  s_seen_hits[OC_MAX_SEEN];

// ---------------------------------------------------------------- geometry

// LVGL has this, but only in src/misc/lv_area_private.h, and a gate that
// reaches into a private header stops building the next time LVGL is bumped.
// Six lines is cheaper than that coupling. Areas are inclusive at both ends.
static bool oc_intersect(lv_area_t *res, const lv_area_t *a, const lv_area_t *b)
{
    res->x1 = a->x1 > b->x1 ? a->x1 : b->x1;
    res->y1 = a->y1 > b->y1 ? a->y1 : b->y1;
    res->x2 = a->x2 < b->x2 ? a->x2 : b->x2;
    res->y2 = a->y2 < b->y2 ? a->y2 : b->y2;
    return res->x1 <= res->x2 && res->y1 <= res->y2;
}

static int area_w(const lv_area_t *a) { return a->x2 - a->x1 + 1; }
static int area_h(const lv_area_t *a) { return a->y2 - a->y1 + 1; }

static bool area_is_backdrop(const lv_area_t *a)
{
    long screen_px = (long)LV_HOR_RES * LV_VER_RES;
    return (long)area_w(a) * area_h(a) * 100 >= screen_px * OC_BACKDROP_PCT;
}

// ---------------------------------------------------------------- text

// Labels carry translated copy, so the report has to survive newlines, very
// long strings and multi byte scripts. Truncation stops on a byte that is not a
// UTF-8 continuation, otherwise a Japanese finding prints as mojibake and reads
// like a second bug.
// A SPANGROUP IS TEXT, and eight checks could not see one. The kit builds six
// of them -- the folded address, a definition row, wt_explain_hi's body and the
// explainer overlay's own -- and every one was invisible to TEXT, CONTENT,
// GROWTH, CLIPPED, BARE, WALL, CUT and the size dump, because all of them ask
// lv_obj_check_type for lv_label_class and a spangroup is not one.
//
// Nothing about those checks cares which class drew the glyphs. They care what
// the string is, how tall its line box is, and whether it wraps -- all three of
// which a spangroup can answer.
static bool oc_is_text(lv_obj_t *o)
{
    return lv_obj_check_type(o, &lv_label_class) ||
           lv_obj_check_type(o, &lv_spangroup_class);
}

// The spans joined, in order. A body split into "sentence" + "." + "sentence"
// is one string to every check that reads it, which is what it is on the glass.
static void oc_span_text(lv_obj_t *o, char *out, size_t out_len)
{
    size_t n = 0;
    uint32_t cnt = lv_spangroup_get_span_count(o);
    for (uint32_t i = 0; i < cnt && n + 1 < out_len; i++) {
        lv_span_t *sp = lv_spangroup_get_child(o, (int32_t)i);
        const char *t = sp ? lv_span_get_text(sp) : NULL;
        for (; t && *t && n + 1 < out_len; t++)
            out[n++] = (*t == '\n' || *t == '\r') ? ' ' : *t;
    }
    out[n] = '\0';
}

static const char *oc_text_of(lv_obj_t *o, char *scratch, size_t len)
{
    if (lv_obj_check_type(o, &lv_label_class)) return lv_label_get_text(o);
    if (lv_obj_check_type(o, &lv_spangroup_class)) {
        oc_span_text(o, scratch, len);
        return scratch;
    }
    return NULL;
}

static void oc_text(lv_obj_t *o, char *out, size_t out_len)
{
    char sp[512];
    const char *t = oc_text_of(o, sp, sizeof sp);
    if (!t || !*t) {
        // "(not text)" told the reader nothing and made every finding about a
        // decoration look the same. Name what it actually hit.
        const char *k = "obj";
        if (lv_obj_check_type(o, &lv_image_class))       k = "image";
        else if (lv_obj_check_type(o, &lv_button_class)) k = "button";
        else if (lv_obj_check_type(o, &lv_arc_class))    k = "arc";
        else if (lv_obj_check_type(o, &lv_line_class))   k = "line";
        else if (lv_obj_check_type(o, &lv_bar_class))    k = "bar";
        else if (lv_obj_get_child_count(o) > 0)          k = "container";
        snprintf(out, out_len, "<%s%s>", k,
                 lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) ? " tappable" : "");
        return;
    }

    size_t n = 0;
    size_t cap = out_len < 41 ? out_len - 1 : 40;
    while (t[n] && n < cap) {
        out[n] = (t[n] == '\n' || t[n] == '\r') ? ' ' : t[n];
        n++;
    }
    while (n > 0 && ((unsigned char)t[n] & 0xC0) == 0x80) n--;
    out[n] = '\0';
    if (t[n]) snprintf(out + n, out_len - n, "%s", "...");
}

static const char *oc_short_tag(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

// The arcade game the signer hides behind is not wallet UI, and the bug class
// this gate exists for cannot happen there: main.c draws the game with LVGL's
// built-in Montserrat over baked art, and its only live text is an ASCII score.
// Nothing in it is translated, so nothing in it can grow when a locale does.
// Checking it produces findings about a HUD that is working as intended.
static bool oc_is_game_frame(const char *tag)
{
    static const char *game[] = {
        "sim_menu_intro.ppm", "sim_menu.ppm",  "sim_saver.ppm",
        "sim_play.ppm",       "sim_swipe.ppm", "sim_over.ppm",
        "sim_playagain.ppm",  "sim_menu_back.ppm", NULL
    };
    const char *base = oc_short_tag(tag);
    for (int i = 0; game[i]; i++)
        if (strcmp(base, game[i]) == 0) return true;
    return false;
}

// ---------------------------------------------------------------- collection

static bool oc_visible(lv_obj_t *o)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    if (lv_obj_get_style_opa(o, LV_PART_MAIN) == LV_OPA_TRANSP) return false;
    return true;
}

static void oc_collect(lv_obj_t *o, lv_area_t clip, bool clip_scrolls)
{
    if (s_n >= OC_MAX_NODES) return;
    if (!oc_visible(o)) return;
    // A decor CONTAINER takes its whole subtree with it, and that is the
    // difference between this and the per-node skip the CONTENT check does.
    // The home motes are leaves and are still collected. The signed screen's
    // arrival motion is not: it is 48 labels stacked three deep at fourteen
    // origins, on purpose, because the faces are fixed pitch and that is how
    // per character colour is drawn without one object per character. All
    // nine questions here are about where content was PUT on a page, and none
    // of them means anything asked of one 122ms frame of an animation -- the
    // page underneath is in this tree too, and it is the one to measure.
    if (wt_is_decor(o) && lv_obj_get_child_count(o) > 0) return;

    lv_area_t coords;
    lv_obj_get_coords(o, &coords);

    lv_area_t vis;
    if (!oc_intersect(&vis, &coords, &clip)) return;   // clipped out entirely

    bool is_label = oc_is_text(o);

    // A label with no text has a box but nothing in it, and comparing empty
    // boxes invents findings nobody can act on.
    if (is_label) {
        char sp[512];
        const char *t = oc_text_of(o, sp, sizeof sp);
        if (!t || !*t) return;
    }

    uint32_t kids = lv_obj_get_child_count(o);

    oc_node_t *n = &s_node[s_n++];
    n->obj       = o;
    n->vis       = vis;
    n->nat       = coords;
    n->lh        = is_label
                 ? (int)lv_font_get_line_height(lv_obj_get_style_text_font(o, LV_PART_MAIN))
                 : 0;
    n->is_label  = is_label;
    // A spangroup in BREAK mode wraps, which is the same claim LONG_MODE_WRAP
    // makes about a label -- and it is what BARE and WALL are asking about.
    n->wraps     = is_label &&
                   (lv_obj_check_type(o, &lv_spangroup_class)
                      ? lv_spangroup_get_mode(o) == LV_SPAN_MODE_BREAK
                      : lv_label_get_long_mode(o) == LV_LABEL_LONG_MODE_WRAP);
    n->leaf      = kids == 0;
    n->clickable = lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE);
    n->buried    = false;
    // Losing the bottom of a label to a container that scrolls is a list, and
    // the reader can bring the rest into view. Losing it to one that does not
    // is text nobody can ever read.
    n->cut       = is_label && coords.y2 > vis.y2 && !clip_scrolls;
    // The same question sideways, and the one that was never asked. `cut` has
    // only ever compared y, so a label WIDER than the box holding it was
    // invisible to every check in this file. The auto-lock banner sat in a
    // 420px box with the English string measuring 420px at font23 and shipped
    // reading "ocking soon. tap to stay open" -- clipped at both ends, on a
    // stop the walk photographs, with every gate green.
    //
    // SCROLL and SCROLL_CIRCULAR are the legitimate case: a label that moves
    // to show the rest is not a label with a missing half. DOT is not exempt
    // here because it never clips -- it rewrites its own text, which is what
    // CUT reports.
    {
        // A spangroup has no long mode. It never marquees and never dots, so
        // WRAP is the honest stand-in -- and reading a label's accessor off
        // one is a segfault, which is how this was found.
        const lv_label_long_mode_t lm =
            lv_obj_check_type(o, &lv_label_class) ? lv_label_get_long_mode(o)
                                                  : LV_LABEL_LONG_MODE_WRAP;
        const bool marquee = lm == LV_LABEL_LONG_MODE_SCROLL
                          || lm == LV_LABEL_LONG_MODE_SCROLL_CIRCULAR;
        n->cutx = is_label && !marquee && !clip_scrolls
               && (coords.x1 < vis.x1 || coords.x2 > vis.x2);
    }
    n->parent    = lv_obj_get_parent(o);

    // Children are clipped to this object unless it says otherwise. This is why
    // a list row scrolled out of view is not reported: its coords are off the
    // screen, so its visible area comes back empty and it never enters the set.
    lv_area_t child_clip = clip;
    bool child_scrolls = clip_scrolls;
    if (!lv_obj_has_flag(o, LV_OBJ_FLAG_OVERFLOW_VISIBLE)) {
        child_clip   = vis;
        child_scrolls = lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    }

    for (uint32_t i = 0; i < kids; i++)
        oc_collect(lv_obj_get_child(o, i), child_clip, child_scrolls);
}

static bool oc_is_ancestor(lv_obj_t *maybe, lv_obj_t *of)
{
    for (lv_obj_t *p = lv_obj_get_parent(of); p; p = lv_obj_get_parent(p))
        if (p == maybe) return true;
    return false;
}

// An explainer card, a language picker or a zoomed QR is painted over the screen
// it came from. The content underneath is still in the object tree with real
// coordinates, so without this every overlay reports a screenful of collisions
// against the thing hiding it.
//
// The first attempt looked for an opaque DIRECT CHILD of the active screen and
// missed every one of them, because screens are built inside containers and an
// overlay lands at whatever depth its parent happens to be. Paint order is the
// property that actually matters and it does not care about depth: LVGL draws a
// parent, then its children in index order, which is exactly the order this
// collection walk appends. So anything appended before a full screen opaque
// object is behind it. Its own ancestors are excluded, since a container does
// not disappear behind its own child.
static void oc_mark_buried(void)
{
    for (int k = 0; k < s_n; k++) {
        if (lv_obj_get_style_bg_opa(s_node[k].obj, LV_PART_MAIN) < LV_OPA_50) continue;
        if (!area_is_backdrop(&s_node[k].vis)) continue;
        for (int i = 0; i < k; i++)
            if (!oc_is_ancestor(s_node[i].obj, s_node[k].obj))
                s_node[i].buried = true;
    }
}

static int oc_bottom(void);

// Does this screen have the standard action row at all?
//
// WT_CONTENT_BOTTOM is a promise the wallet kit makes, not a law of the panel.
// The gesture unlock, the passphrase keyboard and the game all put things in
// the bottom band on their own terms, and measuring them against a line they
// never agreed to would report a screenful of intended layout. So check 2 runs
// only where a button is genuinely sitting in the action band.
static bool oc_has_action_row(void)
{
    for (int i = 0; i < s_n; i++) {
        oc_node_t *n = &s_node[i];
        if (n->buried || !n->clickable) continue;
        if (area_is_backdrop(&n->vis)) continue;      // tap-to-dismiss backdrops
        if (n->vis.y1 >= oc_bottom() && n->vis.y2 < LV_VER_RES) return true;
    }
    return false;
}

// ---------------------------------------------------------------- reporting

// The same layout fault shows up at every stop that draws the screen, and 168
// stops times 21 locales would bury the signal. Each distinct finding prints
// once with the first stop that showed it; repeats are counted instead.
static void oc_report_one(const char *tag, const char *sig, const char *detail)
{
    for (int i = 0; i < s_seen_n; i++)
        if (strcmp(s_seen[i], sig) == 0) { s_seen_hits[i]++; return; }

    s_findings++;
    if (s_seen_n < OC_MAX_SEEN) {
        snprintf(s_seen[s_seen_n], sizeof s_seen[0], "%s", sig);
        s_seen_hits[s_seen_n] = 1;
        s_seen_n++;
    }
    printf("  %s  at %s\n", detail, oc_short_tag(tag));
}

// ---------------------------------------------------------------- the checks

static void oc_check_text_overlap(const char *tag)
{
    char at[64], bt[64], sig[192], detail[320];

    for (int i = 0; i < s_n; i++) {
        if (!s_node[i].is_label || s_node[i].buried) continue;
        for (int j = i + 1; j < s_n; j++) {
            if (!s_node[j].is_label || s_node[j].buried) continue;
            // A label nested inside another label's subtree is that label's own
            // business, not a collision between two pieces of copy.
            if (s_node[j].parent == s_node[i].obj) continue;
            if (s_node[i].parent == s_node[j].obj) continue;

            lv_area_t hit;
            if (!oc_intersect(&hit, &s_node[i].vis, &s_node[j].vis)) continue;
            if (area_w(&hit) < OC_SLOP) continue;
            if (area_h(&hit) < oc_vslop(&s_node[i], &s_node[j])) continue;

            if (getenv("OVERLAPCHECK_DEBUG")) {
                const lv_font_t *fi = lv_obj_get_style_text_font(s_node[i].obj, LV_PART_MAIN);
                const lv_font_t *fj = lv_obj_get_style_text_font(s_node[j].obj, LV_PART_MAIN);
                char ai[64], bi[64];
                oc_text(s_node[i].obj, ai, sizeof ai);
                oc_text(s_node[j].obj, bi, sizeof bi);
                printf("[dbg] TEXT ov %dx%d  lh %d/%d  boxh %d/%d  \"%s\" / \"%s\"\n",
                       area_w(&hit), area_h(&hit),
                       (int)lv_font_get_line_height(fi), (int)lv_font_get_line_height(fj),
                       area_h(&s_node[i].vis), area_h(&s_node[j].vis), ai, bi);
            }

            oc_text(s_node[i].obj, at, sizeof at);
            oc_text(s_node[j].obj, bt, sizeof bt);
            snprintf(sig, sizeof sig, "TEXT|%s|%s", at, bt);
            snprintf(detail, sizeof detail,
                     "TEXT     \"%s\" and \"%s\" share %dx%d px",
                     at, bt, area_w(&hit), area_h(&hit));
            oc_report_one(tag, sig, detail);
        }
    }
}

// The line THIS screen's content may not cross. Almost always
// WT_CONTENT_BOTTOM; WT_ACTION_Y_SLIDE on a screen whose slide grew the band
// upward to hold a 44px knob. Asking the screen rather than assuming the
// constant is the whole point: the band moved, and a check measuring against
// the number it used to be would pass a paragraph running under the bar.
//
// BURIED slides do not count. An explainer overlay covers the page it opened
// over, backdrop and all, and its own OK sits on the standard row -- so its
// content is measured against 398 even though a slide is still down there.
static int oc_bottom(void)
{
    for (int i = 0; i < s_n; i++)
        if (!s_node[i].buried && wt_is_slide_band(s_node[i].obj))
            return WT_ACTION_Y_SLIDE;
    return WT_CONTENT_BOTTOM;
}

static void oc_check_content_bottom(const char *tag)
{
    char t[64], sig[192], detail[320];

    if (!oc_has_action_row()) return;
    const int bottom = oc_bottom();

    for (int i = 0; i < s_n; i++) {
        oc_node_t *n = &s_node[i];
        if (n->buried) continue;
        // A container spans its children by definition and a full screen
        // backdrop crosses every horizontal line, so neither says anything
        // about where content was put.
        if (!n->is_label && !n->leaf) continue;
        if (area_is_backdrop(&n->vis)) continue;
        // Decoration carries nothing to read, so it has nothing to lose to the
        // action bar. The home motes rise the whole height of the screen and
        // would otherwise report this rule against a 4px speck, at whichever
        // tick the walk happened to save the frame. See wt_mark_decor.
        if (wt_is_decor(n->obj)) continue;

        // Starts above the line and finishes at or below it: the definition of
        // reaching into the action row. Something that starts below the line is
        // already in the action row, which is where buttons belong.
        if (n->vis.y1 >= bottom) continue;
        if (n->vis.y2 < bottom) continue;

        oc_text(n->obj, t, sizeof t);
        snprintf(sig, sizeof sig, "CONTENT|%s|%d", t, (int)n->vis.y2);
        snprintf(detail, sizeof detail,
                 "CONTENT  \"%s\" runs y %d..%d, past the band's top %d by %d px",
                 t, (int)n->vis.y1, (int)n->vis.y2, bottom,
                 (int)n->vis.y2 - bottom + 1);
        oc_report_one(tag, sig, detail);
    }
}

static void oc_check_wrap_growth(const char *tag)
{
    char t[64], ot[64], sig[192], detail[320];

    for (int i = 0; i < s_n; i++) {
        oc_node_t *n = &s_node[i];
        if (!n->wraps || n->buried) continue;

        // Only where the parent lays its children out by hand.
        //
        // The check is "taller than the gap to the next ABSOLUTELY POSITIONED
        // sibling", and that qualifier is the whole point: under a flex or grid
        // layout LVGL moves the following sibling down, so a label growing is
        // exactly what is supposed to happen and reporting it is reporting the
        // fix. Converting these screens to flex IS phase 1, so this check
        // measures the screens that have not had it yet and goes quiet on the
        // ones that have.
        if (lv_obj_get_style_layout(n->parent, LV_PART_MAIN) != LV_LAYOUT_NONE) continue;

        // The nearest thing positioned below it, in the same column, under the
        // same parent. Absolute layout means nothing moves out of the way, so
        // that gap is the whole budget the label has when a translation runs
        // long. Sharing a column matters: a sibling off to the right starts
        // lower on the screen without ever being in this label's way.
        oc_node_t *below = NULL;
        for (int j = 0; j < s_n; j++) {
            if (j == i || s_node[j].buried) continue;
            // Text against text. Lapping a few pixels into a decorative object
            // or the edge of a scroll container means nothing, because those
            // carry their own padding and their contents are separate nodes
            // that get checked on their own terms. Measured: every finding
            // this excluded was 3 to 5 px against a container, and the one
            // real collision it also excluded is still reported twice over by
            // the TEXT and CONTENT checks.
            if (!s_node[j].is_label) continue;
            if (s_node[j].parent != n->parent) continue;
            if (s_node[j].vis.y1 <= n->vis.y1) continue;

            lv_area_t col;
            if (!oc_intersect(&col, &s_node[j].vis, &n->vis)) {
                // no vertical overlap yet, so compare the x spans directly
                int x1 = s_node[j].vis.x1 > n->vis.x1 ? s_node[j].vis.x1 : n->vis.x1;
                int x2 = s_node[j].vis.x2 < n->vis.x2 ? s_node[j].vis.x2 : n->vis.x2;
                if (x2 - x1 + 1 < OC_SLOP) continue;
            } else if (area_w(&col) < OC_SLOP) {
                continue;
            }

            if (!below || s_node[j].vis.y1 < below->vis.y1) below = &s_node[j];
        }
        if (!below) continue;
        // Same leading tolerance the TEXT check uses, and for the same reason:
        // a single line caption resting on the value under it shares a few
        // pixels of line box while rendering perfectly.
        if (n->vis.y2 - below->vis.y1 + 1 < oc_vslop(n, below)) continue;

        oc_text(n->obj, t, sizeof t);
        oc_text(below->obj, ot, sizeof ot);
        snprintf(sig, sizeof sig, "GROWTH|%s|%s", t, ot);
        snprintf(detail, sizeof detail,
                 "GROWTH   wrapped \"%s\" ends y %d, into \"%s\" which starts y %d",
                 t, (int)n->vis.y2, ot, (int)below->vis.y1);
        oc_report_one(tag, sig, detail);
    }
}

// Text that was laid out but never drawn.
//
// The plan's three checks all reason about where things ARE, and there is a
// fourth way for a screen to be wrong: a label asks for a box taller than the
// container holding it, the container clips, and the reader gets a line of
// glyphs sliced in half with the rest unreachable. Nothing overlaps and nothing
// crosses the action row, so the other three see a clean screen.
//
// docs/media/sign-verify.png is the case that earned this check. The recipient
// address asks for y 354..411, its column clips at 391, and the render shows a
// row of text cut through the middle just above BACK and DETAILS. This is the
// same root cause as the rest of phase 1, absolute y under content that grows,
// so it belongs to the same gate.
static void oc_check_clipped(const char *tag)
{
    char t[64], sig[192], detail[320];

    for (int i = 0; i < s_n; i++) {
        oc_node_t *n = &s_node[i];
        if (!n->is_label || n->buried || !n->cut) continue;

        oc_text(n->obj, t, sizeof t);
        snprintf(sig, sizeof sig, "CLIPPED|%s", t);
        snprintf(detail, sizeof detail,
                 "CLIPPED  \"%s\" asks for y %d..%d, cut off at %d, %d px unreadable",
                 t, (int)n->nat.y1, (int)n->nat.y2, (int)n->vis.y2,
                 (int)(n->nat.y2 - n->vis.y2));
        oc_report_one(tag, sig, detail);
    }

    for (int i = 0; i < s_n; i++) {
        oc_node_t *n = &s_node[i];
        if (!n->is_label || n->buried || !n->cutx) continue;

        oc_text(n->obj, t, sizeof t);
        snprintf(sig, sizeof sig, "CLIPX|%s", t);
        snprintf(detail, sizeof detail,
                 "CLIPX    \"%s\" asks for x %d..%d, visible only %d..%d, "
                 "%d px cut off the side -- the box is sized to a number and "
                 "the string is wider than it",
                 t, (int)n->nat.x1, (int)n->nat.x2,
                 (int)n->vis.x1, (int)n->vis.x2,
                 (int)((n->vis.x1 - n->nat.x1) + (n->nat.x2 - n->vis.x2)));
        oc_report_one(tag, sig, detail);
    }
}

// ---------------------------------------------------------------- colour roles

// Rule 1 of ADDENDUM-02: a themed accent and a status colour never appear on
// the same element, pick one per object.
//
// sim/themecheck.c answers the palette half of that addendum, whether an accent
// lands on top of a status colour, and it cannot answer this half: "on the same
// element" is a property of a rendered object and themecheck never builds one.
// This gate already holds the tree at every settled screen, so the question
// costs a walk it was doing anyway.
//
// What breaks is hierarchy rather than meaning. A card whose border says
// caution and whose label says suggested action asks the reader to hold two
// colour systems at once, and at arm's length on a 4.3 inch panel both resolve
// to "some coloured chrome". Status then stops standing out, which is the only
// job status has.
//
// MONO is skipped, and that is the addendum's own observation: its accent IS
// WT_INK, so every piece of ordinary text would classify as accent and the
// check would report the entire UI. Nothing can be mistaken for a status there
// because nothing except status is coloured, so there is no question to ask.
#define OC_MAX_COLOURS 10

typedef struct {
    uint32_t    hex;
    const char *where;
} oc_colour_t;

static const struct { const char *name; uint32_t hex; } OC_STATUS[] = {
    { "WT_OK",   0x35D07F },
    { "WT_WARN", 0xF2B84B },
    { "WT_STOP", 0xFF4D5E },
};
#define OC_NSTATUS ((int)(sizeof OC_STATUS / sizeof OC_STATUS[0]))

// A colour drawn at partial opacity is not the colour the eye receives, so
// classify what lands on the panel: the source composited over the page. This
// is what an opacity threshold would have been standing in for, without the
// threshold being a number somebody picked by feel. A quarter opacity accent
// wash resolves to something near WT_BG and stops counting as an accent on its
// own, which is the correct answer and not a tuned one.
//
// The backdrop is approximated as WT_BG rather than whatever surface is really
// underneath. Every surface in the kit (WT_BAR, WT_PANEL, WT_KEY) is within a
// few units of WT_BG by construction, so the approximation costs nothing.
static uint32_t oc_over_bg(lv_color_t c, lv_opa_t opa)
{
    lv_color_t bg = WT_BG;
    int a = opa;
    int r = (c.red   * a + bg.red   * (255 - a)) / 255;
    int g = (c.green * a + bg.green * (255 - a)) / 255;
    int b = (c.blue  * a + bg.blue  * (255 - a)) / 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// Every colour this object actually paints, and the style property each came
// from so a finding says where to look. Text is read only from labels: LVGL
// inherits text colour, so asking a container returns its parent's ink and
// invents an accent on a box that draws no glyphs.
static int oc_colours_of(lv_obj_t *o, bool is_label, oc_colour_t *out, int cap)
{
    int n = 0;
    lv_opa_t opa;

    if (is_label && (opa = lv_obj_get_style_text_opa(o, LV_PART_MAIN)) > 0 && n < cap) {
        out[n].hex = oc_over_bg(lv_obj_get_style_text_color(o, LV_PART_MAIN), opa);
        out[n++].where = "text";
    }
    if ((opa = lv_obj_get_style_bg_opa(o, LV_PART_MAIN)) > 0 && n < cap) {
        out[n].hex = oc_over_bg(lv_obj_get_style_bg_color(o, LV_PART_MAIN), opa);
        out[n++].where = "fill";
    }
    if (lv_obj_get_style_border_width(o, LV_PART_MAIN) > 0 &&
        (opa = lv_obj_get_style_border_opa(o, LV_PART_MAIN)) > 0 && n < cap) {
        out[n].hex = oc_over_bg(lv_obj_get_style_border_color(o, LV_PART_MAIN), opa);
        out[n++].where = "border";
    }
    if (lv_obj_get_style_outline_width(o, LV_PART_MAIN) > 0 &&
        (opa = lv_obj_get_style_outline_opa(o, LV_PART_MAIN)) > 0 && n < cap) {
        out[n].hex = oc_over_bg(lv_obj_get_style_outline_color(o, LV_PART_MAIN), opa);
        out[n++].where = "outline";
    }
    if (lv_obj_get_style_line_width(o, LV_PART_MAIN) > 0 &&
        (opa = lv_obj_get_style_line_opa(o, LV_PART_MAIN)) > 0 && n < cap) {
        out[n].hex = oc_over_bg(lv_obj_get_style_line_color(o, LV_PART_MAIN), opa);
        out[n++].where = "line";
    }
    if (lv_obj_get_style_arc_width(o, LV_PART_MAIN) > 0 &&
        (opa = lv_obj_get_style_arc_opa(o, LV_PART_MAIN)) > 0 && n < cap) {
        out[n].hex = oc_over_bg(lv_obj_get_style_arc_color(o, LV_PART_MAIN), opa);
        out[n++].where = "arc";
    }
    // The filled part of a bar or an arc is a separate style part, and it is
    // usually the coloured one: an entropy meter is WT_OK over a WT_KEY track.
    if ((opa = lv_obj_get_style_bg_opa(o, LV_PART_INDICATOR)) > 0 && n < cap) {
        out[n].hex = oc_over_bg(lv_obj_get_style_bg_color(o, LV_PART_INDICATOR), opa);
        out[n++].where = "indicator";
    }
    if (lv_obj_get_style_arc_width(o, LV_PART_INDICATOR) > 0 &&
        (opa = lv_obj_get_style_arc_opa(o, LV_PART_INDICATOR)) > 0 && n < cap) {
        out[n].hex = oc_over_bg(lv_obj_get_style_arc_color(o, LV_PART_INDICATOR), opa);
        out[n++].where = "arc indicator";
    }
    return n;
}

// How much the ROLE check actually looked at. Printed because "0 findings" and
// "never ran" read identically otherwise: if a walk stops reaching the themed
// screens, these two counts fall to zero and say so, where the finding count
// alone would keep reporting a clean gate.
static int s_role_accent_objs;
static int s_role_status_objs;

// ---- 6. BARE: a screen whose only content is a wall of text ----------------
//
// The product has a kit for this -- wt_card, wt_value_card, wt_facts rows,
// wt_chip, the diagram rows -- and the fault this catches is not using it: a
// title, one 704px grey paragraph and a button. It is not a rendering bug, so
// none of the five checks above can see it; every one of those screens is
// perfectly laid out. It is the layout being wrong to begin with.
//
// It is a GATE and not a note in a document because that is the only kind of
// rule that has held in this tree. The chrome rule has been written down more
// than once and screens kept shipping bare anyway.
//
// A wall is a wrapping label wide enough to be the page's body. A frame is
// anything with a border and a fill big enough to be a card. A screen with a
// wall and no frame is the shape being rejected.
//
// It used to count a why-block's 3px rule bar as well, and on thirteen screens
// that bar was the only frame it could see. The bar is gone -- the body is a
// plain paragraph now -- so those thirteen were reported the moment it went,
// which is what the check is for. They were fixed by cutting the copy until no
// single claim was a wall, not by drawing a box around one.
//
// The thresholds are deliberately generous: 560px is far wider than a 344px
// why-block, and 90px is three lines at font23. Nothing that has been through
// the kit can trip this, so a finding is a real bare screen rather than a
// judgement call about density.
static bool oc_is_frame(const oc_node_t *n)
{
    int w = n->vis.x2 - n->vis.x1 + 1, h = n->vis.y2 - n->vis.y1 + 1;
    if (n->is_label) return false;
    if (area_is_backdrop(&n->vis)) return false;      // the screen's own base
    // A CONTROL IS NOT CHROME. Counting buttons made every screen in the
    // product look furnished and the first run of this check found nothing at
    // all. The question is what the screen puts ABOVE the action row to carry
    // its content, so the action row itself is not an answer to it, and
    // neither is anything else the reader can press.
    if (n->clickable) return false;
    if (n->vis.y2 >= WT_CONTENT_BOTTOM) return false;
    if (w < 100 || h < 30) return false;
    if (lv_obj_get_style_border_width(n->obj, LV_PART_MAIN) < 1) return false;
    if (lv_obj_get_style_bg_opa(n->obj, LV_PART_MAIN) >= LV_OPA_50) return true;
    // A SEVERITY TINTED CARD IS A FRAME TOO, and this test could not see one.
    // wt_row_sev paints its fill at opa 13 under a border at 77 -- the
    // drawing's 5 percent under 30 percent -- so every amber and red card on
    // the device failed the fill test above while being the most framed object
    // on its screen. A screen whose only chrome was a WT_SEV card has always
    // been reported BARE, which is the gate's blind spot rather than the
    // screen's fault.
    //
    // The BORDER is what makes it a frame; the wash is what makes it a
    // warning. So ask whether the border is actually PAINTED, which an
    // invisible box still fails.
    return lv_obj_get_style_border_opa(n->obj, LV_PART_MAIN) >= 50;
}

// The screens that are bare TODAY, queued for the chrome rollout. Each entry is
// here because the screen EXISTS in this state, not because it is acceptable,
// and the list only ever shrinks: delete the line when the screen is rebuilt.
//
// Landing the gate with a backlog rather than waiting until all of them are
// fixed is the whole point. Held back, it protects nothing while the work is in
// progress; landed, it stops screen number nine from ever being written. The
// count of unhit entries is printed at the end of the run, so a stale line
// cannot sit here quietly after its screen has been fixed.
//
// Matched on the stop tag, which is locale independent -- the finding text is
// translated copy and would need twenty one spellings of the same exemption.
static const char *OC_BARE_BACKLOG[] = {
    // EMPTY, and that is the point. Nine stops were listed here when the check
    // landed; all nine were rebuilt around the ruled body, and when that shape
    // was retired they came back and were fixed properly -- by cutting the
    // copy until no single claim is a wall.'
    // A new entry is a screen someone chose not to fix, and needs saying so.
    NULL,   // C forbids an empty initialiser; the loop below skips NULLs
};
static bool s_bare_hit[sizeof OC_BARE_BACKLOG / sizeof OC_BARE_BACKLOG[0]];

static bool oc_bare_excused(const char *tag)
{
    for (unsigned i = 0; i < sizeof OC_BARE_BACKLOG / sizeof OC_BARE_BACKLOG[0]; i++)
        if (OC_BARE_BACKLOG[i] && strstr(tag, OC_BARE_BACKLOG[i]))
            { s_bare_hit[i] = true; return true; }
    return false;
}

static void oc_check_bare(const char *tag)
{
    char t[64], sig[192], detail[320];

    // Only screens that play by the kit's rules in the first place. The game,
    // the keyboard and the gesture unlock have no action row and no obligation
    // to look like a wallet page, which is the same exemption check 2 uses.
    if (!oc_has_action_row()) return;

    const oc_node_t *wall = NULL;
    for (int i = 0; i < s_n; i++) {
        const oc_node_t *n = &s_node[i];
        if (n->buried) continue;
        if (oc_is_frame(n)) return;                   // screen has chrome, done
        if (wall) continue;
        if (!n->is_label || !n->wraps) continue;
        int w = n->vis.x2 - n->vis.x1 + 1, h = n->vis.y2 - n->vis.y1 + 1;
        if (w >= 560 && h >= 90) wall = n;
    }
    if (!wall) return;
    if (oc_bare_excused(tag)) return;

    oc_text(wall->obj, t, sizeof t);
    snprintf(sig, sizeof sig, "BARE|%s", t);
    snprintf(detail, sizeof detail,
             "BARE     \"%s\" is a %dx%d paragraph and the screen has no framed "
             "element (wt_card / wt_value_card / a diagram in one)",
             t, (int)(wall->vis.x2 - wall->vis.x1 + 1),
             (int)(wall->vis.y2 - wall->vis.y1 + 1));
    oc_report_one(tag, sig, detail);
}

// ---- 8. FIT: a fit helper that gave up ------------------------------------
//
// wt_note_fit picks the biggest font that FITS, so a string too long for its
// box comes back at font14 and reports nothing. The source looks correct, the
// gate sees no overlap, and the screen ships with an instruction in the
// smallest type the device owns. That is not a translation
// being long -- it is copy too long for the space, or a layout budget thrown
// away in code, and the house rules say to cut words or move the blocks rather
// than accept the size.
//
// kiss_theme calls the sink below every time either helper reaches that rung.
// The strings land here as the screen is BUILT, which is before the save() that
// checks it, so they are attributed to the next stop -- the one whose build
// they came from. A screen built and never saved has no stop to blame and its
// strings go to whichever stop follows; that is the same blind spot
// check_screen_coverage.py exists to close, not a new one.
#define OC_FIT_MAX 24
static char s_fit_kind[OC_FIT_MAX][8];
static char s_fit_txt[OC_FIT_MAX][96];
static int  s_fit_n;

// Only where font14 is a fault. The house rules keep it for chip labels and
// unit suffixes -- MARKS -- and a fit helper handed a chip-sized box is doing
// exactly its job: "dust attack" in a 110px caution chip is not the bug. A
// BODY that fell to font14 is. (The pill kind is gone with the pills: the
// arrow and word actions never re-font, so only notes can reach this sink.)
//
// Row sub-lines used to be on the exempt list here and in the house rules, and
// they were the wrong thing to exempt: every teaching line on the settings page
// is a row sub-line, and the page came off the bench as text nobody could read.
// They are font23 now, and what goes wrong at that size is an ellipsis rather
// than a rung -- which is check 9, CUT.
//
// 300 is read off the kit, not guessed: the narrowest real body column is
// 330; below that is a chip or a badge.
// The HEIGHT matters as much. A caution row gives its subline about 24px, and
// one line of font23 is 31 -- so font14 there is the box deciding, not the copy,
// and "high fee" is not a screen anybody needs to fix. 36 is one font23 line
// with its leading, which is the least a box can offer and still be a choice.
#define OC_FIT_BODY_W 300
#define OC_FIT_BODY_H  36

static void oc_fit_sink(const char *kind, const char *txt, int w, int h)
{
    if (w < OC_FIT_BODY_W || h < OC_FIT_BODY_H) return;
    if (s_fit_n >= OC_FIT_MAX) return;
    snprintf(s_fit_kind[s_fit_n], sizeof s_fit_kind[0], "%s", kind ? kind : "?");
    snprintf(s_fit_txt[s_fit_n], sizeof s_fit_txt[0], "%s", txt ? txt : "");
    s_fit_n++;
}

// Before main, because the strings are produced while a screen is BUILT and the
// first screen is built before anything in this file is called. There is no
// main() here -- the gate links into the walk -- so a constructor is the only
// hook that runs early enough.
__attribute__((constructor))
static void oc_fit_install(void) { wt_fit_set_sink(oc_fit_sink); }

// Shrink only, like the two above. An entry is a string somebody decided to
// leave at font14, and that decision needs saying so here.
static const char *OC_FIT_BACKLOG[] = {
    // EMPTY. Two strings were listed here on the day this check landed and both
    // were cut rather than excused: the camera-proof warning lost a clause that
    // its own warning triangle was already saying, and the settings pill took
    // the shorter wording the other twenty locales had all along. A new entry
    // is a string somebody chose to leave OVERFLOWING its block at the 21px
    // floor, and needs saying so.
    NULL,   // C forbids an empty initialiser; the loop below skips NULLs
};
static bool s_fit_hit[sizeof OC_FIT_BACKLOG / sizeof OC_FIT_BACKLOG[0]];

static bool oc_fit_excused(const char *txt)
{
    for (unsigned i = 0; i < sizeof OC_FIT_BACKLOG / sizeof OC_FIT_BACKLOG[0]; i++)
        if (OC_FIT_BACKLOG[i] && strstr(txt, OC_FIT_BACKLOG[i]))
            { s_fit_hit[i] = true; return true; }
    return false;
}



// ---- 11. VOID: a screen with almost nothing on it --------------------------
//
// Every other check on this list fires on too MUCH -- a paragraph too wide, a
// frame drawn round a wall of text, a label past the bottom. Nothing fired on
// too LITTLE, so the term takeover pages shipped as a title, a round badge,
// two lines of body, OK, and 260px of nothing, and every gate stayed green.
// BARE cannot see them because their paragraph is three lines short of a wall.
//
// Measured as the fraction of the CONTENT LANE's rows that any visible element
// covers. Rows rather than area, because a screen is read down the page: two
// short lines with a 200px hole under them is the fault, and an area measure
// would score it the same as the same two lines spread out.
//
// EXEMPT BY ELEMENT KIND, NEVER BY SCREEN NAME. A screen whose content is one
// big block -- a QR, the viewfinder, a word grid -- is full by construction
// however little of the lane its rows touch, and a list of exempt screen names
// rots the first time one is renamed. 200x200 is the smallest of those three
// by a wide margin.
#define OC_VOID_MIN_BLOCK 200
#define OC_VOID_FLOOR      40      // percent of the lane's rows

static bool oc_has_severity_ink(void)
{
    for (int i = 0; i < s_n; i++) {
        const oc_node_t *n = &s_node[i];
        if (n->buried) continue;
        if (n->is_label) {
            lv_color_t c = lv_obj_get_style_text_color(n->obj, LV_PART_MAIN);
            if (lv_color_eq(c, WT_WARN) || lv_color_eq(c, WT_STOP)) return true;
        }
        lv_color_t b = lv_obj_get_style_border_color(n->obj, LV_PART_MAIN);
        if (lv_obj_get_style_border_opa(n->obj, LV_PART_MAIN) >= 50 &&
            (lv_color_eq(b, WT_WARN) || lv_color_eq(b, WT_STOP))) return true;
    }
    return false;
}

static int oc_content_coverage(void)
{
    // LV_VER_RES is a call on this build, so the array is sized by a constant
    // comfortably past the panel's 480 rather than by it.
    static bool row[800];
    const int top = 16, bot = WT_CONTENT_BOTTOM;
    for (int y = top; y < bot; y++) row[y] = false;

    for (int i = 0; i < s_n; i++) {
        const oc_node_t *n = &s_node[i];
        if (n->buried) continue;
        if (area_is_backdrop(&n->vis)) continue;
        int w = n->vis.x2 - n->vis.x1 + 1, h = n->vis.y2 - n->vis.y1 + 1;
        if (w <= 1 || h <= 1) continue;
        if (w >= OC_VOID_MIN_BLOCK && h >= OC_VOID_MIN_BLOCK) return 100;
        int y1 = n->vis.y1 < top ? top : n->vis.y1;
        int y2 = n->vis.y2 >= bot ? bot - 1 : n->vis.y2;
        for (int y = y1; y <= y2; y++) row[y] = true;
    }
    int used = 0;
    for (int y = top; y < bot; y++) if (row[y]) used++;
    return used * 100 / (bot - top);
}

// The screens that are sparse TODAY. Landed WITH the backlog rather than held
// back until they are all fixed, which is the argument BARE's own list makes:
// held back it protects nothing while the work is in progress; landed, it stops
// screen number seventeen from ever being written. Shrink only -- delete the
// line when the screen is rebuilt, and the run prints how many are left.
//
// Two kinds went on it and they left by different doors.
//
// The TERM TAKEOVERS are the ones this check was written for -- a title, a
// round badge, two lines and 260px of nothing -- and they are GONE. Both took
// a value card carrying the claim their sentence never got to: what sighash
// ALL prevents once you sign, and who sees a merge and for how long. 36% and
// 32% of the lane became 58% and 54%. Neither needed a new shape, only the
// `cap`/`val` band wt_explain_open has always drawn.
//
// The rest say ONE thing and mean the space: an erase that finished, a card
// that could not be read, a backup whose version is wrong. Their weight is the
// message and filling them would soften a refusal an owner has to take
// seriously. They are here rather than exempted because "deliberately sparse"
// is a judgement, and a judgement in a gate is a carve-out that grows.
static const char *OC_VOID_BACKLOG[] = {
    // one thing, said loudly, with the room to mean it
    "sim_amnesic_qrbad",
    "sim_duress_pick",
    "sim_gword_failed",
    "sim_kef_badver",
    "sim_kef_pick",
    // Covers sim_sd_missing_retry too, and must: the list is matched with
    // strstr, so a shorter entry swallows every tag it prefixes and the longer
    // one could never be marked hit -- which the unmatched-entry report caught
    // the moment both were listed.
    "sim_sd_missing",
    "sim_sign_failed",
    "sim_storage_cleanup",
    "sim_storage_fail",
    "sim_storage_sd_ok",
    "sim_wipe_fail",
    "sim_wiped",
};
static bool s_void_hit[sizeof OC_VOID_BACKLOG / sizeof OC_VOID_BACKLOG[0]];

static bool oc_void_excused(const char *tag)
{
    for (unsigned i = 0; i < sizeof OC_VOID_BACKLOG / sizeof OC_VOID_BACKLOG[0]; i++)
        if (OC_VOID_BACKLOG[i] && strstr(tag, OC_VOID_BACKLOG[i]))
            { s_void_hit[i] = true; return true; }
    return false;
}

static void oc_check_void(const char *tag)
{
    if (!oc_has_action_row()) return;          // same exemption BARE takes
    // A REFUSAL IS ALLOWED TO BE SPARSE, and that is the whole difference
    // between the two lists this check sorts. A screen that says one thing
    // loudly -- erased, rejected, could not read the card -- earns its empty
    // space: the weight IS the message, and filling it would soften a refusal
    // the owner has to take seriously. A term page is neutral teaching and has
    // no such excuse.
    //
    // Asked by COLOUR rather than by name, so it cannot rot: severity ink is
    // what a refusal is made of, and no explainer has any.
    if (oc_has_severity_ink()) return;
    int cov = oc_content_coverage();
    if (getenv("OVERLAPCHECK_VOID"))
        printf("[void] %3d%%  %s\n", cov, oc_short_tag(tag));
    if (cov >= OC_VOID_FLOOR) return;
    if (oc_void_excused(tag)) return;

    char sig[192], detail[320];
    snprintf(sig, sizeof sig, "VOID|%s", oc_short_tag(tag));
    snprintf(detail, sizeof detail,
             "VOID     content covers %d%% of the lane (floor is %d%%): a "
             "title, a little body and a great deal of nothing", cov,
             OC_VOID_FLOOR);
    oc_report_one(tag, sig, detail);
}

// ---- 10. EXIT: a refusal that only goes backwards --------------------------
//
// A screen whose content is an empty state or a refusal, and whose action band
// holds exactly one control which goes BACK. The instruction on such a screen
// is usually right and usually impossible to follow from where the reader is
// standing, so the only thing they can do is leave -- and the sibling that
// would have worked is never named.
//
// SIGN -> SD CARD with an empty slot was the case that named this check. Its
// card said "insert a card holding the PSBT file your coordinator saved" to an
// owner who has no card, and the band said BACK. The way out was SCAN QR,
// which needs no card at all.
//
// AND THE SCREEN HAS NO TAB STRIP, which is the clause that makes the check
// usable. A tab strip IS a way on: it is navigation the band does not carry,
// and every tabbed pane on this device would otherwise be reported the moment
// its band is a lone BACK. Without this clause the first run named a dozen
// screens that are not dead ends at all.
//
// Back is found by its GLYPH, not its word: wt_arrow_action draws WT_ICON_ARR_L
// when back is true, and the word beside it is translated twenty-one ways.
static bool oc_node_has_ancestor(const oc_node_t *n, const lv_obj_t *anc)
{
    for (lv_obj_t *p = n->obj; p; p = lv_obj_get_parent(p))
        if (p == anc) return true;
    return false;
}

static bool oc_ctrl_is_back(const oc_node_t *ctrl)
{
    char t[64];
    for (int i = 0; i < s_n; i++) {
        const oc_node_t *n = &s_node[i];
        if (!n->is_label || n->buried) continue;
        if (!oc_node_has_ancestor(n, ctrl->obj)) continue;
        oc_text(n->obj, t, sizeof t);
        if (strstr(t, WT_ICON_ARR_L)) return true;
    }
    return false;
}

// Clickable anything sitting on the chrome strip row is the tab strip. A trail
// lives on the same row and is NOT clickable, which is exactly the difference
// that matters here: one of them is a way on and the other is a breadcrumb.
static bool oc_has_tabstrip(void)
{
    for (int i = 0; i < s_n; i++) {
        const oc_node_t *n = &s_node[i];
        if (n->buried || !n->clickable) continue;
        if (area_is_backdrop(&n->vis)) continue;
        int mid = (n->vis.y1 + n->vis.y2) / 2;
        if (mid >= WT_CHROME_STRIP_Y && mid <= WT_CHROME_STRIP_Y + WT_BR_H)
            return true;
    }
    return false;
}

// A warn or empty state CARD, and the frame is load bearing rather than
// incidental. A frameless refusal -- the SD CARD info screen's "no card", say
// -- is usually one whose remedy is physical: "put the card in the slot and
// open this screen again" is followable, there is no sibling action to offer,
// and BACK really is all there is. The screens this check is for are the ones
// that FRAME a refusal and then strand the reader inside it.
//
// A warn or empty state card: a frame painted in a severity colour, or one
// holding a word in it. wt_row_sev tints its fill at opa 13 under a border at
// 77, so the BORDER is what has to be asked about -- the same thing oc_is_frame
// learned the hard way two checks above.
static bool oc_warn_card(void)
{
    for (int i = 0; i < s_n; i++) {
        const oc_node_t *n = &s_node[i];
        if (n->buried || !oc_is_frame(n)) continue;
        lv_color_t bc = lv_obj_get_style_border_color(n->obj, LV_PART_MAIN);
        if (lv_color_eq(bc, WT_WARN) || lv_color_eq(bc, WT_STOP)) return true;
        for (int j = 0; j < s_n; j++) {
            const oc_node_t *m = &s_node[j];
            if (!m->is_label || m->buried) continue;
            if (!oc_node_has_ancestor(m, n->obj)) continue;
            lv_color_t tc = lv_obj_get_style_text_color(m->obj, LV_PART_MAIN);
            if (lv_color_eq(tc, WT_WARN) || lv_color_eq(tc, WT_STOP)) return true;
        }
    }
    return false;
}

// Shrink only, like BARE and WALL. An entry is a screen someone chose not to
// give a way on, and needs saying so.
static const char *OC_EXIT_BACKLOG[] = {
    NULL,   // C forbids an empty initialiser; the loop below skips NULLs
};
static bool s_exit_hit[sizeof OC_EXIT_BACKLOG / sizeof OC_EXIT_BACKLOG[0]];

static bool oc_exit_excused(const char *tag)
{
    for (unsigned i = 0; i < sizeof OC_EXIT_BACKLOG / sizeof OC_EXIT_BACKLOG[0]; i++)
        if (OC_EXIT_BACKLOG[i] && strstr(tag, OC_EXIT_BACKLOG[i]))
            { s_exit_hit[i] = true; return true; }
    return false;
}

static void oc_check_exit(const char *tag)
{
    if (!oc_has_action_row()) return;
    if (oc_has_tabstrip()) return;                 // a tab IS a way on
    if (!oc_warn_card()) return;                   // not a refusal

    const oc_node_t *only = NULL;
    int band = 0;
    for (int i = 0; i < s_n; i++) {
        const oc_node_t *n = &s_node[i];
        if (n->buried || !n->clickable) continue;
        if (area_is_backdrop(&n->vis)) continue;
        if (n->vis.y1 < oc_bottom() || n->vis.y2 >= LV_VER_RES) continue;
        // Only the OUTERMOST clickable counts: an arrow action is one control
        // whose labels may be clickable in their own right, and counting both
        // would make every single-control band look like two.
        bool nested = false;
        for (int j = 0; j < s_n; j++) {
            const oc_node_t *m = &s_node[j];
            if (m == n || m->buried || !m->clickable) continue;
            if (m->obj != n->obj && oc_node_has_ancestor(n, m->obj)) nested = true;
        }
        if (nested) continue;
        band++;
        only = n;
    }
    if (band != 1 || !only) return;
    if (!oc_ctrl_is_back(only)) return;
    if (oc_exit_excused(tag)) return;

    // The control's own text is not worth printing: an arrow action is a
    // container and oc_text answers "<container tappable>" for it. What the
    // reader of this finding needs is the SCREEN, which the tag already names.
    char sig[192], detail[320];
    snprintf(sig, sizeof sig, "EXIT|%s", oc_short_tag(tag));
    snprintf(detail, sizeof detail,
             "EXIT     a refusal or empty state whose band goes only "
             "backwards, on a screen with no tab strip to carry the way on");
    oc_report_one(tag, sig, detail);
}

static void oc_check_fit(const char *tag)
{
    for (int i = 0; i < s_fit_n; i++) {
        if (oc_fit_excused(s_fit_txt[i])) continue;
        char sig[192], detail[320];
        snprintf(sig, sizeof sig, "FIT|%s|%s", s_fit_kind[i], s_fit_txt[i]);
        snprintf(detail, sizeof detail,
                 "FIT      %s ran out of ladder on \"%s\" -- the floor is 21 "
                 "and it still overflows: cut the copy or give the block its "
                 "budget back, never the size",
                 strcmp(s_fit_kind[i], "note") == 0 ? "wt_note_fit"
                                                    : "wt_body_font",
                 s_fit_txt[i]);
        oc_report_one(tag, sig, detail);
    }
    s_fit_n = 0;
}

// ---- 7. WALL: a paragraph with a card drawn round it ----------------------
//
// BARE stops at the first frame it finds (oc_check_bare's early return), which
// is the right question for "did anyone reach for the kit at all" and the wrong
// one for what shipped next: three duress teaching screens answered it with
// wt_card(36, 104, 716, 288) wrapped around forty words of wt_wraph. A border
// round a wall of text is still a wall of text, and BARE passed all three.
//
// So WALL asks the question BARE cannot: is every frame on this screen just a
// box drawn AROUND the paragraph? A chip, a badge, a row, a value card or a
// why-block's rule bar all sit beside or above the body rather than containing
// it, so any one of them clears the screen. Only the container-round-the-prose
// shape is left, and that shape is the one being rejected.
//
// Same wall thresholds as BARE (560x90) so the two checks agree on what counts
// as a page's body, and the same action-row exemption.
static const char *OC_WALL_BACKLOG[] = {
    // EMPTY. The three duress stops that this check was written against lead
    // with a diagram now. A new entry is a screen someone chose not to rebuild,
    // and needs saying so here.
    NULL,   // C forbids an empty initialiser; the loop below skips NULLs
};
static bool s_wall_hit[sizeof OC_WALL_BACKLOG / sizeof OC_WALL_BACKLOG[0]];

static bool oc_wall_excused(const char *tag)
{
    for (unsigned i = 0; i < sizeof OC_WALL_BACKLOG / sizeof OC_WALL_BACKLOG[0]; i++)
        if (OC_WALL_BACKLOG[i] && strstr(tag, OC_WALL_BACKLOG[i]))
            { s_wall_hit[i] = true; return true; }
    return false;
}

// Chrome, for WALL's purposes, and deliberately a WIDER net than oc_is_frame.
// BARE's 100x30 floor exists so a stray decoration cannot make a bare screen
// look furnished; here the question is the opposite one -- did the screen put
// anything at all beside the prose -- and a wt_chip is about 60x28 and a grid
// badge is 34x34. Measuring those against BARE's floor would call a diagram of
// six chips "no chrome" and fire on a screen that is entirely picture.
static bool oc_is_chrome(const oc_node_t *n)
{
    int w = n->vis.x2 - n->vis.x1 + 1, h = n->vis.y2 - n->vis.y1 + 1;
    if (n->is_label) return false;
    if (area_is_backdrop(&n->vis)) return false;
    if (n->vis.y2 >= WT_CONTENT_BOTTOM) return false;
    // AN ACTION IS NOT CHROME, same as BARE -- but a wt_chip IS, and LVGL marks
    // both clickable: lv_obj_create sets the flag and wt_chip's
    // lv_obj_remove_style_all does not clear it. Height separates them without
    // this gate having to know about event handlers. Action-row hit targets are
    // WT_ACTION_H tall or taller; a chip is about 33 and a grid badge 34.
    if (n->clickable && h >= WT_ACTION_H) return false;
    if (w <= 4 && h >= 30) return true;               // a why-block's rule bar
    if (w < 20 || h < 16) return false;
    return lv_obj_get_style_border_width(n->obj, LV_PART_MAIN) >= 1 ||
           lv_obj_get_style_bg_opa(n->obj, LV_PART_MAIN) >= LV_OPA_50;
}

// Does the frame's box cover the paragraph's? Inclusive, with 4px of slack:
// a card is drawn a hair outside the text it holds, and LVGL rounds.
static bool oc_frame_holds(const oc_node_t *f, const oc_node_t *w)
{
    return f->vis.x1 <= w->vis.x1 + 4 && f->vis.x2 >= w->vis.x2 - 4 &&
           f->vis.y1 <= w->vis.y1 + 4 && f->vis.y2 >= w->vis.y2 - 4;
}

static void oc_check_wall(const char *tag)
{
    char t[64], sig[192], detail[320];

    if (!oc_has_action_row()) return;

    const oc_node_t *wall = NULL;
    for (int i = 0; i < s_n; i++) {
        const oc_node_t *n = &s_node[i];
        if (n->buried || !n->is_label || !n->wraps) continue;
        int w = n->vis.x2 - n->vis.x1 + 1, h = n->vis.y2 - n->vis.y1 + 1;
        if (w >= 560 && h >= 90) { wall = n; break; }
    }
    if (!wall) return;

    // Any frame that is NOT a box around the paragraph is the screen carrying
    // its content some other way, and that is all this check wants to see.
    int frames = 0;
    for (int i = 0; i < s_n; i++) {
        const oc_node_t *n = &s_node[i];
        if (n->buried || !oc_is_chrome(n)) continue;
        if (!oc_frame_holds(n, wall)) return;
        frames++;
    }
    if (!frames) return;                  // no frame at all: BARE's finding
    if (oc_wall_excused(tag)) return;

    oc_text(wall->obj, t, sizeof t);
    snprintf(sig, sizeof sig, "WALL|%s", t);
    snprintf(detail, sizeof detail,
             "WALL     \"%s\" is a %dx%d paragraph and every frame on the screen "
             "is a box drawn around it (use a diagram row, chips, rows or "
             "wt_facts instead)",
             t, (int)(wall->vis.x2 - wall->vis.x1 + 1),
             (int)(wall->vis.y2 - wall->vis.y1 + 1));
    oc_report_one(tag, sig, detail);
}

// ---- 9. CUT: a sub-line that has dropped words -----------------------------
//
// Row sub-lines are pinned to one line with LV_LABEL_LONG_DOT, so copy too long
// for its lane does not overflow -- it ELLIPSISES, and says nothing. That is
// silent in exactly the way wt_note_fit is silent: the source looks correct,
// every box stays inside every other box, and the screen ships with the second
// half of a sentence replaced by three dots.
//
// It went unseen while the sub-line was font14, because at that size almost
// nothing reached its lane's edge. Lifting it to font23 -- which is what these
// lines are, teaching copy the owner has to read -- made the lanes tight, and
// the settings storage row rendered "on this chip, not encr..." in the first
// frame after the change with every gate green.
//
// The measurement is taken in kiss_theme.c as the label is BUILT, and arrives
// here through a sink, for the same reason the fit strings do: LVGL rewrites
// the label's text to insert the dots, so a gate walking the finished tree
// finds a string that measures exactly one lane wide and no evidence at all.
//
// An ellipsis here means CUT THE COPY. The lane cannot grow -- it is what the
// label's 250px cap and the value chip leave behind, and both of those are load
// bearing. Shrinking the type back is the move this check exists to stop.
#define OC_CUT_MAX 24
static char s_cut_kind[OC_CUT_MAX][8];
static char s_cut_txt[OC_CUT_MAX][96];
static int  s_cut_want[OC_CUT_MAX];
static int  s_cut_lane[OC_CUT_MAX];
static int  s_cut_n;

static void oc_cut_sink(const char *kind, const char *txt, int want, int lane)
{
    if (s_cut_n >= OC_CUT_MAX) return;
    snprintf(s_cut_kind[s_cut_n], sizeof s_cut_kind[0], "%s", kind ? kind : "?");
    snprintf(s_cut_txt[s_cut_n], sizeof s_cut_txt[0], "%s", txt ? txt : "");
    s_cut_want[s_cut_n] = want;
    s_cut_lane[s_cut_n] = lane;
    s_cut_n++;
}

__attribute__((constructor))
static void oc_cut_install(void) { wt_cut_set_sink(oc_cut_sink); }

// Words this device is RIGHT to use at four syllables, and why. Both of them
// are vocabulary the reader will meet outside this box: "coordinator" is what
// every signing device calls the app on the other side of the QR, and
// inventing a shorter word for it would be the house term the glossary exists
// to forbid. "compatible" is the owner's own correction -- "BIP39 signer" is
// not a thing, and "BIP39 compatible signer" is.
//
// This list SHRINKS. A new entry is a word somebody chose to keep, and needs
// saying so here.
static const char *const OC_READ_BACKLOG[] = {
    "coordinator",
    "compatible",
    NULL,
};
static bool s_read_hit[sizeof OC_READ_BACKLOG / sizeof OC_READ_BACKLOG[0]];

// English-only checks ask this. SIM_LANG is the only language input the
// simulator has (main/i18n.c reads no NVS here), so unset means English.
static bool oc_lang_is_en(void)
{
    const char *l = getenv("SIM_LANG");
    return !l || !*l || strncmp(l, "en", 2) == 0;
}
static bool oc_read_excused(const char *w)
{
    for (unsigned i = 0; i < sizeof OC_READ_BACKLOG / sizeof OC_READ_BACKLOG[0]; i++)
        if (OC_READ_BACKLOG[i] && strcmp(OC_READ_BACKLOG[i], w) == 0)
            { s_read_hit[i] = true; return true; }
    return false;
}

// ---- INK: a paragraph's WORDS wearing the accent ---------------------------
//
// The accent is for MARKS: a chip, a chevron, a tick, a row label, and the
// single full stop that ends a sentence. It is never the colour of the words
// themselves, and this check exists because that rule was broken by one line
// and shipped.
//
// wt_gate's sentence and a few other single lines ARE accent by design, so the
// rule cannot be "no accent text". The shape it asks about is exact: a
// paragraph built by the kit's span builder carries WT_FLAG_ACCENT_STOPS, and
// its ordinary runs deliberately carry NO span style so they inherit the
// GROUP's colour -- which is what lets a caller recolour one the way it
// recoloured the label it replaced. So the group's own text colour is the
// colour of every word in it, and if that is the accent then the whole
// paragraph is.
//
// What went wrong was reusing WT_FLAG_ACCENT for the stops. That flag means
// "paint this object's text the accent", not "this object has accent bits in
// it", so the first theme applied turned every sentence on the device the
// accent colour. Nothing caught it: the size and geometry checks do not ask
// about colour, the role gate asks only about accent-versus-status collisions
// on chips and borders, and every frame anyone looked at was rendered in MONO,
// where the accent is a pale grey and the mistake is invisible.
static void oc_check_ink(const char *tag)
{
    const lv_color_t acc = wt_accent();
    for (int i = 0; i < s_n; i++) {
        lv_obj_t *o = s_node[i].obj;
        if (!lv_obj_check_type(o, &lv_spangroup_class)) continue;
        // NOT keyed on WT_FLAG_ACCENT_STOPS. The bug this exists for set the
        // OTHER flag, so a check that only looked at the right one would have
        // watched it go past. Any wrapping spangroup whose GROUP colour is the
        // accent has accent words in it, whatever flag put the colour there.
        if (!s_node[i].wraps) continue;
        if (!lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), acc))
            continue;
        char sp[512];
        const char *t = oc_text_of(o, sp, sizeof sp);
        char sig[192], detail[320];
        snprintf(sig, sizeof sig, "INK|%s", t ? t : "");
        snprintf(detail, sizeof detail,
                 "INK      a paragraph is painted the ACCENT colour, so every "
                 "word in it is: \"%.90s\" -- the accent belongs to marks and "
                 "to the full stop, never to the words",
                 t ? t : "");
        oc_report_one(tag, sig, detail);
    }
}

static void oc_check_cut(const char *tag)
{
    char sig[192], detail[320];
    for (int i = 0; i < s_cut_n; i++) {
        // The kit names what it measured. Every one of these is a label
        // pinned to one line, which is the whole class: LVGL rewrites the
        // text to insert the dots, so nothing downstream can tell an
        // ellipsised string from one that fits exactly.
        // Three of the kinds are not about a lane. They arrive through this
        // sink because they are the same SHAPE of finding -- measured in the
        // kit as the thing is built, invisible to any walk of the finished
        // tree, fixed by cutting copy -- and each says what it measured.
        if (strcmp(s_cut_kind[i], "term") == 0) {
            snprintf(sig, sizeof sig, "TERM|%s", s_cut_txt[i]);
            snprintf(detail, sizeof detail,
                     "TERM     the definition and its TECHNICAL line reach "
                     "y %d in a row that ends at %d -- \"%s\" is a line too "
                     "long for an open row",
                     s_cut_want[i], s_cut_lane[i], s_cut_txt[i]);
            oc_report_one(tag, sig, detail);
            continue;
        }
        // READ IS AN ENGLISH CHECK, and it has to say so or it is noise.
        // Both halves of it are shaped by English: the syllable limit calls
        // "koordinatora" a failure when it is simply the Czech word, and the
        // word limit counts a language that needs more words for the same
        // sentence as worse writing. Neither is fixable by cutting copy,
        // which is the only fix this gate offers, and a gate whose findings
        // nobody can act on is a gate nobody reads.
        //
        // What is lost is nothing: reading level is a property of the SOURCE
        // copy, and the source copy is English. It is checked where it is
        // written.
        if (!oc_lang_is_en() &&
            (strcmp(s_cut_kind[i], "words") == 0 ||
             strcmp(s_cut_kind[i], "long") == 0 ||
             strcmp(s_cut_kind[i], "mark") == 0 ||
             strcmp(s_cut_kind[i], "widow") == 0))
            continue;
        if (strcmp(s_cut_kind[i], "mark") == 0) {
            snprintf(sig, sizeof sig, "MARK|%s", s_cut_txt[i]);
            if (s_cut_lane[i])
                snprintf(detail, sizeof detail,
                         "MARK     the caption \"%s\" is %d words -- a "
                         "caption NAMES the figure under it, and the limit "
                         "is %d because it is drawn at font14",
                         s_cut_txt[i], s_cut_want[i], s_cut_lane[i]);
            else
                snprintf(detail, sizeof detail,
                         "MARK     the caption \"%s\" opens a clause, so it "
                         "is a sentence and not a name -- and it is drawn at "
                         "font14, which is the mark size",
                         s_cut_txt[i]);
            oc_report_one(tag, sig, detail);
            continue;
        }
        if (strcmp(s_cut_kind[i], "words") == 0) {
            snprintf(sig, sizeof sig, "READ|words|%s", s_cut_txt[i]);
            snprintf(detail, sizeof detail,
                     "READ     a %d word sentence in \"%s\" -- the limit is "
                     "%d, and a sentence somebody has to re-read is one that "
                     "failed",
                     s_cut_want[i], s_cut_txt[i], s_cut_lane[i]);
            oc_report_one(tag, sig, detail);
            continue;
        }
        if (strcmp(s_cut_kind[i], "widow") == 0) {
            snprintf(sig, sizeof sig, "WIDOW|%s", s_cut_txt[i]);
            snprintf(detail, sizeof detail,
                     "WIDOW    \"%s\" wraps to two lines and leaves %dpx of "
                     "a %dpx lane on the second -- it is two words too long, "
                     "and nothing in the source says so",
                     s_cut_txt[i], s_cut_want[i], s_cut_lane[i]);
            oc_report_one(tag, sig, detail);
            continue;
        }
        if (strcmp(s_cut_kind[i], "long") == 0) {
            if (oc_read_excused(s_cut_txt[i])) continue;
            snprintf(sig, sizeof sig, "READ|long|%s", s_cut_txt[i]);
            snprintf(detail, sizeof detail,
                     "READ     \"%s\" is %d syllables -- the limit is %d "
                     "outside a TECHNICAL line, where the real terms are as "
                     "long as the standard made them",
                     s_cut_txt[i], s_cut_want[i], s_cut_lane[i]);
            oc_report_one(tag, sig, detail);
            continue;
        }
        const char *what = strcmp(s_cut_kind[i], "label") == 0 ? "row label"
                         : strcmp(s_cut_kind[i], "cap")   == 0 ? "fact caption"
                         : strcmp(s_cut_kind[i], "fact")  == 0 ? "fact value"
                                                               : "sub-line";
        snprintf(sig, sizeof sig, "CUT|%s|%s", s_cut_kind[i], s_cut_txt[i]);
        snprintf(detail, sizeof detail,
                 "CUT      %s \"%s\" wants %dpx of a %dpx lane, so it "
                 "ships ellipsised -- cut the copy, the lane cannot grow",
                 what, s_cut_txt[i], s_cut_want[i], s_cut_lane[i]);
        oc_report_one(tag, sig, detail);
    }
    s_cut_n = 0;
}


// ---- 10. TINY: a hardcoded font14 carrying WORDS ---------------------------
//
// FIT catches a fit helper that GAVE UP. It cannot catch a font that was
// simply written down, and font14 written down is how the SIGNED screen's
// "what to do next" line -- the single most important sentence on it --
// shipped as the smallest text on the page with every gate green.
//
// So this asks the finished tree instead: is anything rendering at font14 that
// a person has to READ? font14 is metadata by house rule -- chip labels, unit
// suffixes, chevrons, MARKS -- so the test is whether the string reads as
// prose rather than as a mark, and the cheapest honest proxy for that is word
// count. Three or more space separated tokens is a sentence; "1.6% of what you
// send" and "0.00060000 BTC" are two, and stay metadata.
//
// It walks the same collected nodes every other check does, so anything the
// walk photographs is covered, including screens no grep of the source would
// group together.
#define OC_TINY_MIN_WORDS 3
#define OC_TINY_MIN_CHARS 14
// An uppercase string this long has stopped being a lane label. Five, because
// the longest real caption on the device is "SOURCE 1 WHAT YOU POINT AT" at
// four plus its number, and the shortest thing that got through was six.
#define OC_TINY_MIN_SHOUT 6

static bool oc_font_is_tiny(const lv_font_t *f)
{
    return f == wt_font14() || f == wt_font_mono14();
}

// Words, not a mark. Only a run containing an ASCII letter counts, so an icon
// glyph, a "#0" and a bare number are all worth zero -- "<icon>  NETWORK FEE"
// is a two word CAPTION and not a sentence, and counting the glyph made it
// three.
static int oc_word_count(const char *t)
{
    int n = 0;
    bool in = false, letter = false;
    for (const unsigned char *p = (const unsigned char *)t; ; p++) {
        const bool sp = *p == ' ' || *p == '\n' || *p == '\t' || *p == '\0';
        if (sp) {
            if (in && letter) n++;
            in = false; letter = false;
            if (!*p) break;
            continue;
        }
        in = true;
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) letter = true;
    }
    return n;
}

// A CAPTION is upper case by house grammar -- "WHAT SURVIVES", "NETWORK FEE",
// "INPUTS (1)" -- and prose is lower case. A caption at font14 is a different
// and smaller problem from a SENTENCE at font14: the reader is scanning for it,
// not reading it. This check is about the second, and a check that fired on
// both would be one nobody reads.
static bool oc_has_lowercase(const char *t)
{
    for (const unsigned char *p = (const unsigned char *)t; *p; p++)
        if (*p >= 'a' && *p <= 'z') return true;
    return false;
}

// Shrink only, like the others. An entry is a font14 string somebody decided
// to leave at that size, and that decision needs saying so here.
static const char *OC_TINY_BACKLOG[] = {
    NULL,   // C forbids an empty initialiser; the loop below skips NULLs
};
static bool s_tiny_hit[sizeof OC_TINY_BACKLOG / sizeof OC_TINY_BACKLOG[0]];

static bool oc_tiny_excused(const char *txt)
{
    for (unsigned i = 0; i < sizeof OC_TINY_BACKLOG / sizeof OC_TINY_BACKLOG[0]; i++)
        if (OC_TINY_BACKLOG[i] && strstr(txt, OC_TINY_BACKLOG[i]))
            { s_tiny_hit[i] = true; return true; }
    return false;
}

// Every ellipsis on the device, found the same way a reader finds one: by the
// dots.
//
// CUT measures at the call site, through a sink, because LVGL rewrites the
// label's own text and a gate reading the finished tree sees a string exactly
// one lane wide. That is true of the WIDTH -- and the rewritten text still
// ends in the dots it was given, which the tree does show. So CUT stays (it
// names the lane and the overflow, which is what tells you how much copy to
// cut) and this asks the cheaper question everywhere at once: is anything on
// this screen wearing an ellipsis. No call site has to be wired, so a row
// helper nobody remembered cannot hide one -- which is exactly what wt_row_x
// did until this morning.
static bool oc_ends_in_dots(const char *t)
{
    size_t n = strlen(t);
    if (n >= 3 && strcmp(t + n - 3, "...") == 0) return true;
    return n >= 3 && strcmp(t + n - 3, "\xE2\x80\xA6") == 0;   // U+2026
}

static void oc_check_dots(const char *tag)
{
    char t[96], sig[192], detail[320];
    for (int i = 0; i < s_n; i++) {
        const oc_node_t *n = &s_node[i];
        if (n->buried || !n->is_label) continue;
        // DOTS is a label idiom; a spangroup cannot be in it.
        if (!lv_obj_check_type(n->obj, &lv_label_class)) continue;
        if (lv_label_get_long_mode(n->obj) != LV_LABEL_LONG_MODE_DOTS) continue;
        const char *txt = lv_label_get_text(n->obj);
        if (!txt || !*txt || !oc_ends_in_dots(txt)) continue;
        oc_text(n->obj, t, sizeof t);
        snprintf(sig, sizeof sig, "DOTS|%s", t);
        snprintf(detail, sizeof detail,
                 "DOTS     \"%s\" is wearing an ellipsis -- its second half is "
                 "gone and nothing in the source says so. Cut the copy: the "
                 "lane is what the label and the value beside it leave behind",
                 t);
        oc_report_one(tag, sig, detail);
    }
}

static void oc_check_tiny(const char *tag)
{
    char t[96], sig[192], detail[320];
    for (int i = 0; i < s_n; i++) {
        const oc_node_t *n = &s_node[i];
        if (n->buried || !n->is_label) continue;
        char spbuf[512];
        const char *txt = oc_text_of(n->obj, spbuf, sizeof spbuf);
        if (!txt || !*txt) continue;
        if (!oc_font_is_tiny(lv_obj_get_style_text_font(n->obj, LV_PART_MAIN)))
            continue;
        // Declared metadata: a unit suffix, a counter, a corner diagnostic.
        // The claim is made at the call site beside its reason, which is where
        // a reader can check it -- a backlog of strings here could not be
        // traced back to a screen by anybody.
        if (lv_obj_has_flag(n->obj, WT_FLAG_TINY_OK)) continue;
        // Uppercase is a CAPTION lane -- scanned, not read -- but only while
        // it is caption length. "THE BACKLIGHT IS THE PROGRESS BAR" is six
        // words and an instruction, and it sat at font14 on the screen that
        // goes dark for a minute because this line skipped every capital
        // string there was.
        if (!oc_has_lowercase(txt) && oc_word_count(txt) < OC_TINY_MIN_SHOUT)
            continue;
        if (oc_word_count(txt) < OC_TINY_MIN_WORDS) continue;
        if ((int)strlen(txt) < OC_TINY_MIN_CHARS) continue;
        if (oc_tiny_excused(txt)) continue;
        oc_text(n->obj, t, sizeof t);
        snprintf(sig, sizeof sig, "TINY|%s", t);
        snprintf(detail, sizeof detail,
                 "TINY     \"%s\" is a sentence rendered at font14 -- the size "
                 "was written in, so no fit helper could report it. font14 is "
                 "for MARKS; anything an owner reads sits at 23 or better",
                 t);
        oc_report_one(tag, sig, detail);
    }
}


// ---- 11. AMBER: WT_WARN carrying WORDS -------------------------------------
//
// Amber is a MARK colour on this device, not an ink. The bench looked at a
// finished sweep and said so: "all the yellow text... i wanna see more theme
// color... the only yellow thing i wanna see is caution symbols and yellow
// floating/pulsing dots".
//
// That is a rule a gate can hold. A caution GLYPH stays amber, a breathing dot
// stays amber, and everything an owner READS takes the accent -- which is the
// colour they chose, and the one that makes a page look like their device.
//
// Same word test TINY uses, and for the same reason: a lone LV_SYMBOL_WARNING
// is a mark and must not report, while "not real bitcoin" is a sentence and
// must. A label with no ASCII letter at all is a glyph.
static bool oc_is_warn(lv_color_t c)
{
    lv_color32_t p = lv_color_to_32(c, LV_OPA_COVER);
    return p.red == 0xF2 && p.green == 0xB8 && p.blue == 0x4B;
}

static const char *OC_AMBER_BACKLOG[] = {
    NULL,   // C forbids an empty initialiser; the loop below skips NULLs
};
static bool s_amber_hit[sizeof OC_AMBER_BACKLOG / sizeof OC_AMBER_BACKLOG[0]];

static bool oc_amber_excused(const char *txt)
{
    for (unsigned i = 0; i < sizeof OC_AMBER_BACKLOG / sizeof OC_AMBER_BACKLOG[0]; i++)
        if (OC_AMBER_BACKLOG[i] && strstr(txt, OC_AMBER_BACKLOG[i]))
            { s_amber_hit[i] = true; return true; }
    return false;
}

static void oc_check_amber(const char *tag)
{
    char t[96], sig[192], detail[320];
    for (int i = 0; i < s_n; i++) {
        const oc_node_t *n = &s_node[i];
        if (n->buried || !n->is_label) continue;
        char spbuf[512];
        const char *txt = oc_text_of(n->obj, spbuf, sizeof spbuf);
        if (!txt || !*txt) continue;
        if (!oc_is_warn(lv_obj_get_style_text_color(n->obj, LV_PART_MAIN)))
            continue;
        if (oc_word_count(txt) < 1) continue;      // a bare glyph is a MARK
        if (oc_amber_excused(txt)) continue;
        oc_text(n->obj, t, sizeof t);
        snprintf(sig, sizeof sig, "AMBER|%s", t);
        snprintf(detail, sizeof detail,
                 "AMBER    \"%s\" is WORDS in WT_WARN -- amber is a mark colour "
                 "here: the caution GLYPH and the breathing dot keep it, and "
                 "anything read takes wt_accent()",
                 t);
        oc_report_one(tag, sig, detail);
    }
}

static void oc_check_colour_roles(const char *tag)
{
    char t[64], sig[192], detail[320];

    lv_color_t ac = wt_accent();
    uint32_t ahex = ((uint32_t)ac.red << 16) | ((uint32_t)ac.green << 8) | ac.blue;
    lv_color_t ink = WT_INK;
    uint32_t inkhex = ((uint32_t)ink.red << 16) | ((uint32_t)ink.green << 8) | ink.blue;
    // GREEN only. This used to skip MONO as well, whose accent was WT_INK to
    // the byte -- so the one theme with no accent was also the one theme with
    // no colour supervision. MONO has a real accent now and is checked like
    // the other three; the guard stays for the case it was really about, which
    // is an accent a reader cannot tell from the ink beside it.
    if (cde_same(ahex, inkhex)) return;

    for (int i = 0; i < s_n; i++) {
        oc_node_t *n = &s_node[i];
        if (n->buried) continue;

        oc_colour_t col[OC_MAX_COLOURS];
        int nc = oc_colours_of(n->obj, n->is_label, col, OC_MAX_COLOURS);
        if (nc < 1) continue;

        for (int c = 0; c < nc; c++) {
            if (cde_same(col[c].hex, ahex)) { s_role_accent_objs++; break; }
        }
        for (int c = 0; c < nc; c++) {
            int hit = 0;
            for (int s = 0; s < OC_NSTATUS && !hit; s++)
                if (cde_same(col[c].hex, OC_STATUS[s].hex)) hit = 1;
            if (hit) { s_role_status_objs++; break; }
        }
        if (nc < 2) continue;

        for (int a = 0; a < nc; a++) {
            if (!cde_same(col[a].hex, ahex)) continue;
            for (int b = 0; b < nc; b++) {
                if (b == a) continue;
                // The two have to be TELLABLE APART to compete. In GREEN the
                // accent is WT_OK to the byte, so an accent fill beside a WT_OK
                // glyph renders as one colour and there is nothing for a reader
                // to resolve. Only a second, visibly different colour that
                // carries status meaning is a finding.
                if (cde_same(col[b].hex, col[a].hex)) continue;
                for (int s = 0; s < OC_NSTATUS; s++) {
                    if (!cde_same(col[b].hex, OC_STATUS[s].hex)) continue;

                    oc_text(n->obj, t, sizeof t);
                    snprintf(sig, sizeof sig, "ROLE|%s|%s|%s|%s",
                             wt_accent_name(), t, col[a].where, OC_STATUS[s].name);
                    snprintf(detail, sizeof detail,
                             "ROLE     \"%s\" wears the %s accent (%s #%06X) and %s "
                             "(%s #%06X) at once",
                             t, wt_accent_name(), col[a].where, (unsigned)col[a].hex,
                             OC_STATUS[s].name, col[b].where, (unsigned)col[b].hex);
                    oc_report_one(tag, sig, detail);
                }
            }
        }
    }
}

// Does the ROLE check still fire?
//
// It reports nothing on the current UI, which is the answer everyone wants and
// also the answer a check that silently stopped working gives. The other four
// checks are self evidencing: they found faults, the faults were fixed, and a
// regression brings the finding back. This one landed clean, so on its own it
// is indistinguishable from a no-op, and the day someone dresses a caution card
// in accent chrome is the day it has to earn its place.
//
// So it gets three cases built out of real theme colours. The first must fire.
// The second and third must not, and they are the two ways this check could
// have been written to fire on everything: GREEN's accent IS WT_OK, so a rule
// that compared roles without asking whether the colours are tellable apart
// would report every verified glyph in that theme, and MONO's accent is WT_INK,
// so a rule that skipped the MONO exclusion would report every label on the
// device.
//
// Run: OVERLAPCHECK_SELFTEST=1 /tmp/kissoverlap
int oc_selftest(void);

// ---- 15. LADDER: a mark set rungs below the words it belongs to ------------
//
// The device has five faces and they are a LADDER: 14 for marks, then 21, 23,
// 28 and 34 for things people read. A mark is allowed to be smaller than the
// line it leads -- that is what makes it a mark -- but a font14 glyph beside
// a font34 sentence is not a mark, it is a speck, and no check on this file's
// list could see it: every one of those labels fits its box perfectly and
// nothing overlaps.
//
// So this compares SIBLINGS. A label with no ASCII letters is a mark; the
// tallest label sharing its parent is the line it belongs to; three rungs
// between them is the fault. Two is the ordinary case -- a font23 mark beside
// a font34 headline is the explainer's own shape -- and one is everywhere.
#define OC_LADDER_GAP 4
// How close a mark has to be to count as leading a line. A caption lane is
// 38px from its glyph, so 44 covers every mark the kit places and nothing on
// the other side of a column.
#define OC_LADDER_NEAR 44

static int oc_rung(const lv_font_t *f)
{
    if (f == wt_font14()      || f == wt_font_mono14()) return 0;
    if (f == wt_font_mono18())                         return 1;
    if (f == wt_font_mono21())                         return 2;
    if (f == wt_font23()      || f == wt_font_mono23()) return 3;
    if (f == wt_font28()      || f == wt_font_mono28()) return 4;
    if (f == wt_font34()      || f == wt_font_mono34()) return 5;
    return -1;                       // off the ladder: not this check's job
}

// A MARK carries no ASCII letter -- an icon glyph, an arrow, a bullet. The
// same test TINY uses for the opposite purpose, and for the same reason: it
// is the cheapest honest line between a picture and a word.
static bool oc_is_mark(const char *t)
{
    for (const char *p = t; *p; p++)
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) return false;
    return t[0] != 0;
}

static void oc_check_ladder(const char *tag)
{
    char t[80], u[80], sig[224], detail[420];

    for (int i = 0; i < s_n; i++) {
        const oc_node_t *n = &s_node[i];
        if (n->buried || !n->is_label) continue;
        oc_text(n->obj, t, sizeof t);
        if (!oc_is_mark(t)) continue;
        const int mr = oc_rung(lv_obj_get_style_text_font(n->obj, LV_PART_MAIN));
        if (mr < 0) continue;

        // The line the mark BELONGS to, which is not "anything under the same
        // parent": on a screen every label shares the screen, so that test
        // measured a corner glyph against the page title. It is the label
        // this one leads -- vertically overlapping it, and within a mark's
        // own width of its edge.
        int best = -1;
        int bi = -1;
        for (int j = 0; j < s_n; j++) {
            if (j == i || s_node[j].buried || !s_node[j].is_label) continue;
            const oc_node_t *m = &s_node[j];
            if (m->vis.y2 < n->vis.y1 || m->vis.y1 > n->vis.y2) continue;
            const int gap = m->vis.x1 >= n->vis.x2 ? m->vis.x1 - n->vis.x2
                          : n->vis.x1 >= m->vis.x2 ? n->vis.x1 - m->vis.x2
                                                   : 0;
            if (gap > OC_LADDER_NEAR) continue;
            oc_text(m->obj, u, sizeof u);
            if (oc_is_mark(u)) continue;
            const int r =
                oc_rung(lv_obj_get_style_text_font(m->obj, LV_PART_MAIN));
            if (r > best) { best = r; bi = j; }
        }
        if (bi < 0 || best - mr < OC_LADDER_GAP) continue;
        oc_text(s_node[bi].obj, u, sizeof u);
        snprintf(sig, sizeof sig, "LADDER|%s|%s", t, u);
        snprintf(detail, sizeof detail,
                 "LADDER   a mark %d rungs under \"%s\" -- a glyph that small "
                 "beside words that big is a speck, not a mark",
                 best - mr, u);
        oc_report_one(tag, sig, detail);
    }
}

// ---- 12. RAGGED: sibling rows whose sub-lines disagree about size ----------
//
// The tall row's sub is sized by the FIT ladder, which picks the biggest rung
// that fits. Alone that is right. In a LIST it makes type size a function of
// how long each string happens to be, so identical cards land a rung apart and
// the shortest sentence on the page is drawn the largest -- which reads as
// emphasis nobody meant.
//
// It shipped on three screens at once and the bench found all three by eye:
// SET UP THIS SIGNER ("the latter is too big"), WHERE TO KEEP YOUR SEED WORDS
// ("with AMNESIC its too big wtf") and RESTORE. No gate could see it, because
// every one of those labels fits its box perfectly -- there is nothing wrong
// with any row on its own. The defect only exists BETWEEN rows, which is why
// this check compares them rather than measuring them.
//
// The group is "rows of the same size on the same screen": same width, same
// height, taller than WT_ROW_H so the sub is a paragraph rather than a pinned
// caption. Those are siblings by construction -- WT_CHOICE_W/H builds every
// chooser on the device -- and siblings share one rung. wt_row_sub_font is
// how a caller obeys.
#define OC_RAGGED_MAX 24

static void oc_check_ragged(const char *tag)
{
    struct { int w, h; const lv_font_t *f; char t[80]; } g[OC_RAGGED_MAX];
    int ng = 0;
    char t[80], sig[224], detail[420];

    for (int i = 0; i < s_n; i++) {
        const oc_node_t *n = &s_node[i];
        if (n->buried || !n->is_label) continue;
        if (!wt_is_row_sub(n->obj)) continue;
        lv_obj_t *row = lv_obj_get_parent(n->obj);
        if (!row) continue;
        const int rw = lv_obj_get_width(row), rh = lv_obj_get_height(row);
        // A WT_ROW_H row pins its sub to one line at a declared font14 and
        // says so; that is the BOX deciding and TINY covers it. Only the
        // wrapping paragraph is sized by the ladder, so only it can be ragged.
        if (rh <= WT_ROW_H) continue;
        const lv_font_t *f = lv_obj_get_style_text_font(n->obj, LV_PART_MAIN);
        oc_text(n->obj, t, sizeof t);

        int seen = -1;
        for (int k = 0; k < ng; k++)
            if (g[k].w == rw && g[k].h == rh) { seen = k; break; }
        if (seen < 0) {
            if (ng >= OC_RAGGED_MAX) continue;
            g[ng].w = rw; g[ng].h = rh; g[ng].f = f;
            snprintf(g[ng].t, sizeof g[ng].t, "%s", t);
            ng++;
            continue;
        }
        if (g[seen].f == f) continue;
        snprintf(sig, sizeof sig, "RAGGED|%s|%s", g[seen].t, t);
        snprintf(detail, sizeof detail,
                 "RAGGED   \"%s\" and \"%s\" are sub-lines of two %dx%d rows on "
                 "one screen and render at DIFFERENT sizes -- the fit ladder "
                 "sized each by its own length, so the shortest sentence is "
                 "drawn the biggest. Size the group once with wt_row_sub_font "
                 "and pass it as every row's sf",
                 g[seen].t, t, rw, rh);
        oc_report_one(tag, sig, detail);
    }
}


// ---- 10. STALE: the accent a screen kept after the theme moved -------------
//
// wt_accent_set changes the accent with the screen already up, and
// wt_accent_restyle repaints every FLAGGED object under it. Anything wearing
// the accent WITHOUT a flag keeps the old colour, and nothing has ever noticed:
// a build, a rendered frame and the three-accent sweep at the bottom of
// run_overlapcheck.sh all BUILD the screen under one accent and never change
// it. The only thing that has ever caught one is a person looking at a screen
// they had just switched the theme on.
//
// Three were live on the sign screen at once:
//
//   the folded address's lit tail   a spangroup -- the SPANS carry the colour,
//                                   and a text colour on the group is invisible
//   the change row's word           an accent BAKED into recolor markup, which
//                                   no flag can reach
//   the TESTNET chip                wt_state_chip, so every state chip there is
//
// The first two are invisible to oc_colours_of, which reads an object's own
// style properties and neither of those keeps its colour there. That is the
// whole reason this is not four lines inside the ROLE check.
//
// IT ASKS ABOUT THE OLD COLOUR, NOT ABOUT FLAGS. "Accent-coloured means
// flagged" was the first rule written here and it reported 86 objects across
// the walk, because the theme control lives on the Settings band and REBUILDS
// its page -- so for most screens the flag buys nothing and its absence is not
// a defect. What is load bearing is the handful of screens that take the change
// in place, and those are exactly the stops that FOLLOW one: the accent at this
// stop differs from the accent at the last, so anything still wearing the old
// one is a thing the restyle did not reach. No rule to keep in step with the
// product, and no backlog of things nobody is going to fix.
static int        s_stale_prev = -1;
static uint32_t   s_stale_prev_hex;

// Shrink only, like BARE and WALL. One entry, and it is a CHOICE rather than
// an oversight: kiss_sign.c carries the reasoning in full. The flag was tried
// on the sign header's key mark and accent_walk repainting that RECOLOR label
// inside a 236px flex chip crashed on every non-MONO accent. What it costs is
// a mark in the old accent until the next rebuild, which repaint_verify does
// on every acknowledgement and every page turn -- and that was judged the
// smaller of the two.
static const char *OC_STALE_BACKLOG[] = {
    // The key is "<frame>|<text>". Two things an entry written by eye gets
    // wrong: the frame keeps its .ppm, which is what oc_short_tag hands back,
    // and the text OPENS WITH THE GLYPH -- a printed report shows U+F084 as
    // blank, so it reads as leading spaces and matches nothing.
    "sim_sign_accent.ppm|" WT_ICON_KEY "  #7A869C SIGNING AS#",
    NULL,
};
static bool s_stale_hit[sizeof OC_STALE_BACKLOG / sizeof OC_STALE_BACKLOG[0]];

static bool oc_stale_excused(const char *key)
{
    for (unsigned i = 0; i < sizeof OC_STALE_BACKLOG / sizeof OC_STALE_BACKLOG[0]; i++)
        if (OC_STALE_BACKLOG[i] && strstr(key, OC_STALE_BACKLOG[i]))
            { s_stale_hit[i] = true; return true; }
    return false;
}

static void oc_stale_report(const char *tag, lv_obj_t *o, const char *where,
                            const char *why)
{
    char t[96], key[224], sig[192], detail[400];
    oc_text(o, t, sizeof t);
    snprintf(key, sizeof key, "%s|%s", oc_short_tag(tag), t);
    if (oc_stale_excused(key)) return;
    snprintf(sig, sizeof sig, "STALE|%s|%s", t, where);
    snprintf(detail, sizeof detail,
             "STALE    \"%s\" still wears the OLD accent in its %s after the "
             "theme changed with this screen up -- %s",
             t, where, why);
    oc_report_one(tag, sig, detail);
}

static void oc_check_stale(const char *tag)
{
    const int cur = wt_accent_get();
    const lv_color_t ac = wt_accent();
    const uint32_t curhex = ((uint32_t)ac.red << 16) | ((uint32_t)ac.green << 8) | ac.blue;
    const int prev = s_stale_prev;
    const uint32_t oldhex = s_stale_prev_hex;
    s_stale_prev = cur;
    s_stale_prev_hex = curhex;
    if (prev < 0 || prev == cur) return;          // no change to be stale from

    // MONO's accent is WT_INK, and half the device is legitimately WT_INK.
    // A status colour is the same trap from the other side -- GREEN's accent
    // is WT_OK to the byte, which the theme gate declares.
    if (cde_same(oldhex, ((uint32_t)WT_INK.red << 16) |
                         ((uint32_t)WT_INK.green << 8) | WT_INK.blue)) return;
    for (int k = 0; k < OC_NSTATUS; k++)
        if (cde_same(oldhex, OC_STATUS[k].hex)) return;

    char hex[8];
    snprintf(hex, sizeof hex, "#%02X%02X%02X",
             (unsigned)(oldhex >> 16) & 0xFF, (unsigned)(oldhex >> 8) & 0xFF,
             (unsigned)oldhex & 0xFF);

    for (int i = 0; i < s_n; i++) {
        oc_node_t *n = &s_node[i];
        if (n->buried) continue;
        lv_obj_t *o = n->obj;

        oc_colour_t col[OC_MAX_COLOURS];
        const int nc = oc_colours_of(o, n->is_label, col, OC_MAX_COLOURS);
        for (int c = 0; c < nc; c++)
            if (cde_same(col[c].hex, oldhex))
                oc_stale_report(tag, o, col[c].where,
                                "wt_accent_restyle did not reach it");

        // A SPANGROUP keeps its colours in the spans, so a text colour on the
        // group is invisible. accent_walk repaints the LAST span, which is the
        // lit tail of every address on the device by construction -- but only
        // when the group carries the flag.
        if (lv_obj_check_type(o, &lv_spangroup_class)) {
            const uint32_t sn = lv_spangroup_get_span_count(o);
            for (uint32_t k = 0; k < sn; k++) {
                lv_span_t *sp = lv_spangroup_get_child(o, (int32_t)k);
                lv_style_value_t v;
                if (!sp || lv_style_get_prop(lv_span_get_style(sp),
                                             LV_STYLE_TEXT_COLOR, &v)
                               != LV_STYLE_RES_FOUND) continue;
                const uint32_t h = ((uint32_t)v.color.red << 16) |
                                   ((uint32_t)v.color.green << 8) | v.color.blue;
                if (cde_same(h, oldhex))
                    oc_stale_report(tag, o, "span",
                                    "no flag reaches a spangroup's spans");
            }
        }

        // MARKUP. `#RRGGBB text#` inside a recolor label is a colour written
        // into a STRING, and nothing repaints a string -- so there is no flag
        // that fixes this one. The way out is the one kiss_theme.c states in
        // its own words: put the accent on the OBJECT, and pin the markup to a
        // colour that never moves.
        if (n->is_label && lv_label_get_recolor(o)) {
            const char *txt = lv_label_get_text(o);
            if (txt && strstr(txt, hex))
                oc_stale_report(tag, o, "markup",
                                "an accent baked into a string cannot be repainted");
        }
    }
}

static void oc_check_layer(const char *tag);   // defined with the entry points

static int oc_selftest_case(const char *name, int accent,
                            lv_color_t fill, lv_color_t border,
                            bool want_finding)
{
    wt_accent_set(accent);

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_screen_load(scr);
    lv_obj_set_style_bg_color(scr, WT_BG, LV_PART_MAIN);

    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, 300, 100);
    lv_obj_set_pos(card, 40, 40);
    lv_obj_set_style_bg_color(card, fill, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, border, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_refr_now(NULL);

    s_n = 0;
    s_findings = 0;
    s_seen_n = 0;
    lv_area_t full = { 0, 0, LV_HOR_RES - 1, LV_VER_RES - 1 };
    oc_collect(scr, full, false);
    oc_mark_buried();
    oc_check_colour_roles("selftest");

    bool got = s_findings > 0;
    printf("  %-46s %s (%d finding%s)\n", name,
           got == want_finding ? "ok" : "FAILED", s_findings,
           s_findings == 1 ? "" : "s");
    return got == want_finding ? 0 : 1;
}


// STALE fires only on a stop that FOLLOWS an accent change, which the walk
// does in exactly two places -- so a clean sweep proves nothing about it
// unless the check is shown to still report. Two labels in the accent, one
// flagged and one not: the flagged one must survive the change and the bare
// one must be caught. A check that fired on everything would fail the first
// case exactly as a dead one fails the second.
static int oc_selftest_stale(const char *name, bool flagged, bool want_finding)
{
    wt_accent_set(WT_ACC_PINK);
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_screen_load(scr);
    lv_obj_set_style_bg_color(scr, WT_BG, LV_PART_MAIN);
    lv_obj_t *l = wt_lbl(scr, "ACCENTED", 40, 40, wt_font23(), wt_accent());
    if (flagged) lv_obj_add_flag(l, WT_FLAG_ACCENT);
    lv_refr_now(NULL);

    // Prime the check with the accent this screen was BUILT under, the way a
    // preceding stop would, then change it exactly as the walk does.
    s_stale_prev = -1;
    s_n = 0; s_findings = 0; s_seen_n = 0;
    lv_area_t full = { 0, 0, LV_HOR_RES - 1, LV_VER_RES - 1 };
    oc_collect(scr, full, false);
    oc_mark_buried();
    oc_check_stale("selftest-prime");

    wt_accent_set(WT_ACC_ORANGE);
    wt_accent_restyle(scr);
    lv_refr_now(NULL);
    s_n = 0; s_findings = 0; s_seen_n = 0;
    oc_collect(scr, full, false);
    oc_mark_buried();
    oc_check_stale("selftest");

    bool got = s_findings > 0;
    printf("  %-46s %s (%d finding%s)\n", name,
           got == want_finding ? "ok" : "FAILED", s_findings,
           s_findings == 1 ? "" : "s");
    wt_accent_set(WT_ACC_MONO);
    s_stale_prev = -1;
    return got == want_finding ? 0 : 1;
}

// WALL fires on a shape the product no longer contains, so a clean run over the
// walk proves nothing about it. Same problem the ROLE check has, same answer:
// build the shape here and check the gate still says so.
//
// The arrow action gives the synthetic screen an action row, which is the
// exemption both BARE and WALL share.
static int oc_selftest_wall(const char *name, bool with_chip, bool want_finding)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_screen_load(scr);
    lv_obj_set_style_bg_color(scr, WT_BG, LV_PART_MAIN);

    lv_obj_t *card = wt_card(scr, 36, 104, 716, 288);
    (void)card;
    lv_obj_t *body = wt_wraph(scr,
        "a paragraph long enough to be a page's body, wide enough to be its "
        "only content, and tall enough that nobody reads it twice. this is the "
        "shape a card drawn round prose leaves behind.", 52, 118, 688, 260);
    (void)body;
    if (with_chip) {
        lv_obj_t *row = wt_diagram_row(scr);
        lv_obj_set_pos(row, 48, 60);
        wt_chip(row, "SOMETHING", false);
    }
    wt_arrow_action(scr, "OK", true, false, 300, WT_ACTION_Y, 200, false,
                    NULL, NULL);
    lv_refr_now(NULL);

    s_n = 0;
    s_findings = 0;
    s_seen_n = 0;
    lv_area_t full = { 0, 0, LV_HOR_RES - 1, LV_VER_RES - 1 };
    oc_collect(scr, full, false);
    oc_mark_buried();
    oc_check_wall("selftest");

    bool got = s_findings > 0;
    printf("  %-46s %s (%d finding%s)\n", name,
           got == want_finding ? "ok" : "FAILED", s_findings,
           s_findings == 1 ? "" : "s");
    return got == want_finding ? 0 : 1;
}


// A row needs a callback to wear a chevron, and the chevron is what the lane
// arithmetic subtracts. It is never fired.
static void oc_noop_cb(lv_event_t *e) { (void)e; }

// RAGGED fires on a shape the product no longer contains, so it carries the
// same burden WALL, CUT and LAYER do: a clean sweep proves nothing until the
// check is shown to still fire. Two chooser rows whose sublines are a rung
// apart MUST report; the same two sized as a group MUST NOT -- a check that
// fired on every list would fail the second exactly as a dead one fails the
// first.
//
// The long string is one no rung above 23 can fit in a WT_CHOICE lane and the
// short one fits at 28, which is precisely the pairing that shipped.
static int oc_selftest_ragged(const char *name, bool share, bool want_finding)
{
    static const char *LONG_S =
        "a sentence long enough that the fit ladder cannot draw it at the "
        "largest rung inside a chooser row, so it settles a size lower";
    static const char *SHORT_S = "short enough to sit big";
    const char *const SUBS[2] = { LONG_S, SHORT_S };

    lv_obj_t *scr = wt_screen(NULL, "SELFTEST", NULL);
    lv_screen_load(scr);
    const lv_font_t *f = share
        ? wt_row_sub_font(SUBS, 2, WT_CHOICE_W, WT_CHOICE_H, true, true)
        : NULL;
    for (int i = 0; i < 2; i++)
        wt_row_x(scr, LV_SYMBOL_LIST, "OPTION", SUBS[i], f, NULL, NULL,
                 WT_INK, false, WT_CHOICE_X, WT_CHOICE_Y(i),
                 WT_CHOICE_W, WT_CHOICE_H, oc_noop_cb, NULL);
    lv_refr_now(NULL);

    // s_n FIRST. Without it oc_collect appends to the previous case's nodes and
    // oc_mark_buried hides these rows under them, so the check reports nothing
    // and the self test passes itself by accident.
    s_n = 0;
    s_findings = 0;
    s_seen_n = 0;
    lv_area_t full = { 0, 0, LV_HOR_RES - 1, LV_VER_RES - 1 };
    oc_collect(scr, full, false);
    oc_mark_buried();
    oc_check_ragged("selftest");

    bool got = s_findings > 0;
    printf("  %-46s %s (%d finding%s)\n", name,
           got == want_finding ? "ok" : "FAILED", s_findings,
           s_findings == 1 ? "" : "s");
    return got == want_finding ? 0 : 1;
}

// CUT reports nothing today, which is the same standing WALL has: the shape it
// looks for is one the product no longer contains. A clean sweep means nothing
// without proof the check still fires, so this builds a wide row whose sub is
// far too long for its lane and asserts the sink saw it -- and a second with a
// short one, so a check that fired on everything would fail too.
static int oc_selftest_cut(const char *name, const char *label,
                           const char *sub, bool want_finding)
{
    lv_obj_t *scr = wt_screen(NULL, "SELFTEST", NULL);
    s_cut_n = 0;
    wt_row_wide(scr, WT_WIDE_Y(0), &(wt_wide_t){
        .label = label,
        .sub   = sub,
        .kind  = WT_WIDE_CYCLE,
        .val   = "VALUE",
    });
    lv_refr_now(NULL);

    bool got = s_cut_n > 0;
    printf("  %-46s %s (%d finding%s)\n", name,
           got == want_finding ? "ok" : "FAILED", s_cut_n,
           s_cut_n == 1 ? "" : "s");
    s_cut_n = 0;
    return got == want_finding ? 0 : 1;
}

// FIT hears wt_body_font now, which is where every tall row's sub-line and
// every wt_wraph body is sized. Built at the gate's own floor -- 340 wide by
// 40 tall, just over OC_FIT_BODY_W/H -- so the case proves the report AND the
// size filter in one shape.
static int oc_selftest_fit(const char *name, const char *body,
                           bool want_finding)
{
    lv_obj_t *scr = wt_screen(NULL, "SELFTEST", NULL);
    s_fit_n = 0;
    wt_wraph(scr, body, 48, 118, 340, 40);
    lv_refr_now(NULL);

    bool got = s_fit_n > 0;
    printf("  %-46s %s (%d finding%s)\n", name,
           got == want_finding ? "ok" : "FAILED", s_fit_n,
           s_fit_n == 1 ? "" : "s");
    s_fit_n = 0;
    return got == want_finding ? 0 : 1;
}

// WIDOW goes through the kit, not the sink: the whole check is the wrap
// simulation in kiss_theme.c, and calling the sink directly would prove only
// that a printf works. So it builds the real explainer paragraph -- one body
// that leaves a stub on its second line, one that fills both -- and asks
// whether the measurement saw the difference.
static int oc_selftest_widow(const char *name, const char *para,
                             bool want_finding)
{
    lv_obj_t *scr = wt_screen(NULL, "SELFTEST", NULL);
    s_cut_n = 0;
    wt_explain(scr, "HEADLINE", para, NULL, 0);
    lv_refr_now(NULL);

    int widows = 0;
    for (int i = 0; i < s_cut_n; i++)
        if (strcmp(s_cut_kind[i], "widow") == 0) widows++;
    printf("  %-46s %s (%d finding%s)\n", name,
           (widows > 0) == want_finding ? "ok" : "FAILED", widows,
           widows == 1 ? "" : "s");
    s_cut_n = 0;
    return (widows > 0) == want_finding ? 0 : 1;
}

// TINY fires on a shape the product no longer contains, which is the standing
// WALL and CUT have. Three cases, because this check has three ways to be
// wrong: it must report a lower case sentence at font14, it must NOT report an
// upper case CAPTION at font14 (a caption is scanned, not read, and a check
// that fired on both would be one nobody reads), and it must NOT report a
// sentence that has been DECLARED metadata at its call site.
static int oc_selftest_tiny(const char *name, const char *txt, bool declare,
                            bool want_finding)
{
    lv_obj_t *scr = wt_screen(NULL, "SELFTEST", NULL);
    // LOADED, then rendered: oc_collect reads the coordinates LVGL computed on
    // the last refresh, and a screen that was never on the display has none.
    lv_screen_load(scr);
    lv_obj_t *l = wt_lbl(scr, txt, 48, 118, wt_font14(), WT_MUT);
    if (declare) wt_tiny_ok(l);
    lv_refr_now(NULL);

    s_n = 0; s_findings = 0; s_seen_n = 0;
    lv_area_t full = { 0, 0, LV_HOR_RES - 1, LV_VER_RES - 1 };
    oc_collect(scr, full, false);
    oc_mark_buried();
    oc_check_tiny("selftest");

    bool got = s_findings > 0;
    printf("  %-46s %s (%d finding%s)\n", name,
           got == want_finding ? "ok" : "FAILED", s_findings,
           s_findings == 1 ? "" : "s");
    return got == want_finding ? 0 : 1;
}

// CLIPX fires on a shape the product no longer contains, the standing WALL,
// CUT and TINY have. Two cases, because this check has two ways to be wrong: a
// label wider than the box holding it must report, and one that fits must not.
// The first is the auto-lock banner exactly as it shipped -- a 420px box with
// a centred label the English string overflows at both ends.
static int oc_selftest_clipx(const char *name, const char *txt,
                             int box_w, bool want_finding)
{
    lv_obj_t *scr = wt_screen(NULL, "SELFTEST", NULL);
    lv_screen_load(scr);
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, box_w, 56);
    lv_obj_set_pos(box, (800 - box_w) / 2, 8);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = wt_lbl(box, txt, 0, 0, wt_font23(), WT_WARN);
    lv_obj_center(l);
    lv_refr_now(NULL);

    s_n = 0; s_findings = 0; s_seen_n = 0;
    lv_area_t full = { 0, 0, LV_HOR_RES - 1, LV_VER_RES - 1 };
    oc_collect(scr, full, false);
    oc_mark_buried();
    oc_check_clipped("selftest");

    bool got = s_findings > 0;
    printf("  %-46s %s (%d finding%s)\n", name,
           got == want_finding ? "ok" : "FAILED", s_findings,
           s_findings == 1 ? "" : "s");
    return got == want_finding ? 0 : 1;
}



// VOID reports only backlogged screens today, so it needs proving both ways.
// Two cases: a title with two lines under it and nothing else must fire, and
// the same screen with a card filling the lane must not.
static int oc_selftest_void(const char *name, bool with_card, bool want_finding)
{
    lv_obj_t *scr = wt_screen(NULL, "SELFTEST", NULL);
    lv_screen_load(scr);
    wt_lbl(scr, "two short lines and then nothing at all", 48, 160,
           wt_font23(), WT_MUT);
    if (with_card) {
        // 190 tall, deliberately under OC_VOID_MIN_BLOCK on its short side, so
        // this case proves the COVERAGE arithmetic rather than the big-block
        // exemption sitting in front of it.
        lv_obj_t *c = wt_card(scr, WT_LANE_X, 196, WT_LANE_W, 190);
        wt_lbl(c, "a framed figure", 24, 20, wt_font28(), WT_INK);
    }
    wt_arrow_action(scr, "OK", true, false, 592, WT_ACTION_Y, 160,
                    true, NULL, NULL);
    lv_refr_now(NULL);

    s_n = 0; s_findings = 0; s_seen_n = 0;
    lv_area_t full = { 0, 0, LV_HOR_RES - 1, LV_VER_RES - 1 };
    oc_collect(scr, full, false);
    oc_mark_buried();
    oc_check_void("selftest");

    bool got = s_findings > 0;
    printf("  %-46s %s (%d finding%s)\n", name,
           got == want_finding ? "ok" : "FAILED", s_findings,
           s_findings == 1 ? "" : "s");
    return got == want_finding ? 0 : 1;
}

// EXIT fires on a shape the product no longer contains, so it needs proving
// both ways. Two cases: a refusal whose band is a lone BACK must report, and
// the SAME screen with a tab strip must not -- the strip is the way on, and a
// check without that clause reports every tabbed pane on the device.
static int oc_selftest_exit(const char *name, bool with_tabs, bool want_finding)
{
    lv_obj_t *scr = wt_screen(NULL, "SELFTEST", NULL);
    lv_screen_load(scr);
    if (with_tabs) {
        static const wt_tab_t tabs[2] = {
            { WT_ICON_QR, "ONE", false, false },
            { WT_ICON_SD, "TWO", false, false },
        };
        wt_tabs_flex(scr, tabs, 2, 1, NULL);
    }
    lv_obj_t *card = wt_card(scr, WT_LANE_X, 140, WT_LANE_W, 200);
    lv_obj_set_style_border_color(card, WT_WARN, 0);
    wt_lbl(card, "nothing on this card", 28, 26, wt_font28(), WT_WARN);
    wt_arrow_action(scr, "BACK", true, false, 592, WT_ACTION_Y, 160,
                    true, NULL, NULL);
    lv_refr_now(NULL);

    s_n = 0; s_findings = 0; s_seen_n = 0;
    lv_area_t full = { 0, 0, LV_HOR_RES - 1, LV_VER_RES - 1 };
    oc_collect(scr, full, false);
    oc_mark_buried();
    oc_check_exit("selftest");

    bool got = s_findings > 0;
    printf("  %-46s %s (%d finding%s)\n", name,
           got == want_finding ? "ok" : "FAILED", s_findings,
           s_findings == 1 ? "" : "s");
    return got == want_finding ? 0 : 1;
}

// DOTS fires on a shape the product no longer contains. Two cases: a name too
// long for its lane must report, and one that fits must not.
static int oc_selftest_dots(const char *name, const char *txt, int w,
                            bool want_finding)
{
    lv_obj_t *scr = wt_screen(NULL, "SELFTEST", NULL);
    lv_screen_load(scr);
    lv_obj_t *l = wt_lbl(scr, txt, 48, 118, wt_font23(), WT_MUT);
    lv_obj_set_width(l, w);
    lv_obj_set_height(l, lv_font_get_line_height(wt_font23()));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_refr_now(NULL);

    s_n = 0; s_findings = 0; s_seen_n = 0;
    lv_area_t full = { 0, 0, LV_HOR_RES - 1, LV_VER_RES - 1 };
    oc_collect(scr, full, false);
    oc_mark_buried();
    oc_check_dots("selftest");

    bool got = s_findings > 0;
    printf("  %-46s %s (%d finding%s)\n", name,
           got == want_finding ? "ok" : "FAILED", s_findings,
           s_findings == 1 ? "" : "s");
    return got == want_finding ? 0 : 1;
}

// AMBER fires on a shape the product no longer contains. Two cases: a word in
// WT_WARN must report, and a lone caution GLYPH in WT_WARN must not -- the
// glyph is exactly what the rule keeps amber, so a check that reported both
// would be one nobody could act on.
static int oc_selftest_amber(const char *name, const char *txt,
                             bool want_finding)
{
    lv_obj_t *scr = wt_screen(NULL, "SELFTEST", NULL);
    lv_screen_load(scr);
    wt_lbl(scr, txt, 48, 118, wt_font23(), WT_WARN);
    lv_refr_now(NULL);

    s_n = 0; s_findings = 0; s_seen_n = 0;
    lv_area_t full = { 0, 0, LV_HOR_RES - 1, LV_VER_RES - 1 };
    oc_collect(scr, full, false);
    oc_mark_buried();
    oc_check_amber("selftest");

    bool got = s_findings > 0;
    printf("  %-46s %s (%d finding%s)\n", name,
           got == want_finding ? "ok" : "FAILED", s_findings,
           s_findings == 1 ? "" : "s");
    return got == want_finding ? 0 : 1;
}

// LADDER and the two reading-level checks all fire on shapes the product no
// longer contains, so a clean sweep proves nothing about them until they have
// been shown to report at all.
//
// LADDER builds the shape directly -- a font14 glyph beside a font34 line --
// because the defect is a RELATIONSHIP between two labels and there is no
// kit call that produces it on purpose any more.
static int oc_selftest_ladder(const char *name, const lv_font_t *mark_f,
                              bool want_finding)
{
    lv_obj_t *scr = wt_screen(NULL, "SELFTEST", NULL);
    lv_screen_load(scr);
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 48, 118);
    lv_obj_set_size(box, 600, 60);
    wt_lbl(box, LV_SYMBOL_WARNING, 0, 12, mark_f, WT_WARN);
    wt_lbl(box, "NEVER CHECKED", 34, 0, wt_font34(), WT_INK);
    lv_refr_now(NULL);

    s_n = 0; s_findings = 0; s_seen_n = 0;
    lv_area_t full = { 0, 0, LV_HOR_RES - 1, LV_VER_RES - 1 };
    oc_collect(scr, full, false);
    oc_mark_buried();
    oc_check_ladder("selftest");

    bool got = s_findings > 0;
    printf("  %-46s %s (%d finding%s)\n", name,
           got == want_finding ? "ok" : "FAILED", s_findings,
           s_findings == 1 ? "" : "s");
    return got == want_finding ? 0 : 1;
}

// The two reading-level cases go through the SINK, not through a screen: the
// measure is a pure function of a string and the sink is what carries it, so
// driving the string is driving the whole check.
static int oc_selftest_read(const char *name, const char *kind,
                            const char *txt, int want, bool want_finding)
{
    s_cut_n = 0; s_findings = 0; s_seen_n = 0;
    oc_cut_sink(kind, txt, want, 0);
    oc_check_cut("selftest");
    bool got = s_findings > 0;
    printf("  %-46s %s (%d finding%s)\n", name,
           got == want_finding ? "ok" : "FAILED", s_findings,
           s_findings == 1 ? "" : "s");
    return got == want_finding ? 0 : 1;
}

// MARK goes through a real wt_value_card, not through the sink: the rule IS
// the measure -- a word count and a list of words that cannot begin a name --
// so driving the sink would prove only that the reporter still prints. The
// two that fire are the two strings that came off the bench, and the two that
// must not are the string that fixed one of them and the longest caption on
// the device that was always right.
static int oc_selftest_mark(const char *name, const char *cap,
                            bool want_finding)
{
    lv_obj_t *scr = wt_screen(NULL, "SELFTEST", NULL);
    lv_screen_load(scr);
    s_cut_n = 0; s_findings = 0; s_seen_n = 0;
    wt_value_card(scr, cap, "A VALUE", 48, 118, 704, true);
    lv_refr_now(NULL);
    oc_check_cut("selftest");

    bool got = s_findings > 0;
    printf("  %-46s %s (%d finding%s)\n", name,
           got == want_finding ? "ok" : "FAILED", s_findings,
           s_findings == 1 ? "" : "s");
    return got == want_finding ? 0 : 1;
}

// INK builds the exact shape it forbids and the exact shape it must ignore:
// a kit paragraph whose group colour is the accent, and the same paragraph in
// body ink. Both through wt_body_para, so the check is exercised against what
// the kit actually makes rather than a hand-built spangroup that might drift
// from it.
static int oc_selftest_ink(const char *name, bool accent, bool want_finding)
{
    lv_obj_t *scr = wt_screen(NULL, "SELFTEST", NULL);
    lv_screen_load(scr);
    wt_body_para(scr, "One sentence. And a second one after it.", 120);
    if (accent) {
        // What the bug was: the paragraph flagged so its stops follow the
        // theme, and the flag chosen being the one that paints the whole
        // object's text.
        for (uint32_t i = 0; i < lv_obj_get_child_count(scr); i++) {
            lv_obj_t *c = lv_obj_get_child(scr, i);
            if (lv_obj_check_type(c, &lv_spangroup_class))
                lv_obj_set_style_text_color(c, wt_accent(), 0);
        }
    }
    lv_refr_now(NULL);

    s_n = 0; s_findings = 0; s_seen_n = 0;
    lv_area_t full = { 0, 0, LV_HOR_RES - 1, LV_VER_RES - 1 };
    oc_collect(scr, full, false);
    oc_mark_buried();
    oc_check_ink("selftest");

    bool got = s_findings > 0;
    printf("  %-46s %s (%d finding%s)\n", name,
           got == want_finding ? "ok" : "FAILED", s_findings,
           s_findings == 1 ? "" : "s");
    return got == want_finding ? 0 : 1;
}

int oc_selftest(void)
{
    lv_color_t stop = WT_STOP, ok = WT_OK, ink = WT_INK, key = WT_KEY;
    int bad = 0;

    printf("CUT check self test\n");
    bad += oc_selftest_cut("a sub far longer than its lane, fires", "Label",
                           "a sub-line so long that no lane on this page could "
                           "ever hold it at font23", true);
    bad += oc_selftest_cut("a sub that fits, clear", "Label",
                           "not encrypted", false);
    // The LABEL lane, added the day "Encrypted backup" was found shipping as
    // "Encrypted ba..." with every gate green: CUT measured the sub-line and
    // not the 250px label cap beside it, so half this check was missing.
    bad += oc_selftest_cut("a label far longer than its 250px cap, fires",
                           "A row label with far too many words in it to fit",
                           NULL, true);
    bad += oc_selftest_cut("a label that fits, clear", "STORAGE", NULL, false);
    if (bad) printf("CUT self test: %d case(s) wrong\n", bad);
    else     printf("CUT self test: 4 cases, all as expected\n");
    printf("\n");

    int was_ink = bad;
    printf("INK check self test\n");
    bad += oc_selftest_ink("a paragraph painted the accent, fires", true, true);
    bad += oc_selftest_ink("the same paragraph in body ink, clear", false,
                           false);
    if (bad != was_ink) printf("INK self test: %d case(s) wrong\n",
                               bad - was_ink);
    else                printf("INK self test: 2 cases, all as expected\n");
    printf("\n");

    // Each block reports its OWN verdict, because the marker the run script
    // greps for is what makes a clean sweep mean anything -- a block that
    // prints "all as expected" whatever happened is a marker that says only
    // that the binary got this far.
    int was = bad;
    printf("LADDER check self test\n");
    bad += oc_selftest_ladder("a font14 mark beside a font34 line, fires",
                              wt_font14(), true);
    bad += oc_selftest_ladder("a font23 mark beside the same line, clear",
                              wt_font23(), false);
    if (bad != was) printf("LADDER self test: %d case(s) wrong\n", bad - was);
    else            printf("LADDER self test: 2 cases, all as expected\n");
    printf("\n");

    was = bad;
    printf("VOID check self test\n");
    bad += oc_selftest_void("a title, two lines and nothing else, fires",
                            false, true);
    bad += oc_selftest_void("the same screen with a framed card, clear",
                            true, false);
    if (bad != was) printf("VOID self test: %d case(s) wrong\n", bad - was);
    else            printf("VOID self test: 2 cases, all as expected\n");
    printf("\n");

    was = bad;
    printf("EXIT check self test\n");
    bad += oc_selftest_exit("a refusal whose band is only BACK, fires",
                            false, true);
    bad += oc_selftest_exit("the same refusal with a tab strip, clear",
                            true, false);
    if (bad != was) printf("EXIT self test: %d case(s) wrong\n", bad - was);
    else            printf("EXIT self test: 2 cases, all as expected\n");
    printf("\n");

    was = bad;
    printf("DOTS check self test\n");
    bad += oc_selftest_dots("a name too long for its lane, fires",
                            "zzzz-MANY-recipients-export.psbt", 120, true);
    bad += oc_selftest_dots("the same lane, a name that fits, clear",
                            "ok.psbt", 120, false);
    if (bad != was) printf("DOTS self test: %d case(s) wrong\n", bad - was);
    else            printf("DOTS self test: 2 cases, all as expected\n");
    printf("\n");

    was = bad;
    printf("CLIPX check self test\n");
    bad += oc_selftest_clipx("the auto-lock banner as it shipped, fires",
                             "locking soon. tap to stay open.", 420, true);
    bad += oc_selftest_clipx("the same words in a box that holds them, clear",
                             "locking soon. tap to stay open.", 780, false);
    if (bad != was) printf("CLIPX self test: %d case(s) wrong\n", bad - was);
    else            printf("CLIPX self test: 2 cases, all as expected\n");
    printf("\n");

    was = bad;
    printf("READ check self test\n");
    bad += oc_selftest_read("a 16 word sentence, fires", "words",
                            "one two three four five six seven eight nine ten "
                            "eleven twelve thirteen fourteen fifteen sixteen",
                            16, true);
    bad += oc_selftest_read("a five syllable word, fires", "long",
                            "deterministic", 5, true);
    bad += oc_selftest_read("a word on the backlog, clear", "long",
                            "coordinator", 4, false);
    if (bad != was) printf("READ self test: %d case(s) wrong\n", bad - was);
    else            printf("READ self test: 3 cases, all as expected\n");
    printf("\n");

    was = bad;
    printf("MARK check self test\n");
    bad += oc_selftest_mark("a caption opening a clause, fires",
                            "ONCE YOU SIGN", true);
    bad += oc_selftest_mark("a caption that is a question, fires",
                            "WHO CAN SEE IT", true);
    // The COUNT half, on its own. Both bench strings open a clause, so
    // without this the length limit could be dead and every case above would
    // still say ok -- which is the whole reason this self test exists.
    bad += oc_selftest_mark("five words and no clause word, fires",
                            "TOTAL AMOUNT SENT TO THEM", true);
    // ...and the four word NAME the count used to red at three, which is the
    // string that set the limit. It ships on the firmware signature screen.
    bad += oc_selftest_mark("a four word name, clear",
                            "VERSION ON THE CARD", false);
    bad += oc_selftest_mark("the caption that fixed it, clear",
                            "THIS PAYMENT", false);
    bad += oc_selftest_mark("the longest one that was always right, clear",
                            "USED OF TOTAL", false);
    if (bad != was) printf("MARK self test: %d case(s) wrong\n", bad - was);
    else            printf("MARK self test: 6 cases, all as expected\n");
    printf("\n");

    was = bad;
    printf("WIDOW check self test\n");
    // Two lines with three words on the second, which is the shape that came
    // off the bench, and two lines that both fill their lane.
    // The exact string that came off the bench, in the exact lane it was in.
    bad += oc_selftest_widow("a two line body with a stub second, fires",
                             "Keys come from your seed words and passphrase.",
                             true);
    bad += oc_selftest_widow("a two line body that fills both, clear",
                             "You type the words off your paper. The signer "
                             "checks them against these keys.", false);
    if (bad != was) printf("WIDOW self test: %d case(s) wrong\n", bad - was);
    else            printf("WIDOW self test: 2 cases, all as expected\n");
    printf("\n");

    was = bad;
    printf("TERM check self test\n");
    bad += oc_selftest_read("a body past its row's floor, fires", "term",
                            "a definition whose technical line lands past the "
                            "row", 153, true);
    if (bad != was) printf("TERM self test: %d case(s) wrong\n", bad - was);
    else            printf("TERM self test: 1 case, all as expected\n");
    printf("\n");

    printf("AMBER check self test\n");
    bad += oc_selftest_amber("words in WT_WARN, fires", "not real bitcoin",
                             true);
    bad += oc_selftest_amber("a lone caution glyph in WT_WARN, clear",
                             LV_SYMBOL_WARNING, false);
    if (bad) printf("AMBER self test: %d case(s) wrong\n", bad);
    else     printf("AMBER self test: 2 cases, all as expected\n");
    printf("\n");

    printf("TINY check self test\n");
    bad += oc_selftest_tiny("a lower case sentence at font14, fires",
                            "the words on your paper are the only way back",
                            false, true);
    bad += oc_selftest_tiny("an upper case caption at font14, clear",
                            "WHAT SURVIVES THIS", false, false);
    bad += oc_selftest_tiny("a sentence declared as metadata, clear",
                            "7.0 sat/vB, 1.6% of what you send", true, false);
    if (bad) printf("TINY self test: %d case(s) wrong\n", bad);
    else     printf("TINY self test: 3 cases, all as expected\n");
    printf("\n");

    printf("RAGGED check self test\n");
    bad += oc_selftest_ragged("two chooser rows sized apart, fires", false, true);
    bad += oc_selftest_ragged("the same two sized as a group, clear", true, false);
    if (bad) printf("RAGGED self test: %d case(s) wrong\n", bad);
    else     printf("RAGGED self test: 2 cases, all as expected\n");
    printf("\n");

    printf("FIT check self test\n");
    // Same standing as CUT and WALL: the FIT sink now hears wt_body_font as
    // well as wt_note_fit, and a sweep that reports nothing proves nothing
    // until the new half is shown to fire. A body too long for its box must
    // report; the same box with short copy must not.
    bad += oc_selftest_fit("a body too long for its box, fires",
                           "a body sentence long enough that neither font28 "
                           "nor font23 can fit it into the box below, which "
                           "is what drops it to font14 and says nothing",
                           true);
    bad += oc_selftest_fit("a body that fits, clear", "short enough", false);
    if (bad) printf("FIT self test: %d case(s) wrong\n", bad);
    else     printf("FIT self test: 2 cases, all as expected\n");
    printf("\n");

    printf("WALL check self test\n");
    bad += oc_selftest_wall("card wrapped round a paragraph, fires", false, true);
    bad += oc_selftest_wall("same paragraph with a chip beside it, clear", true, false);
    if (bad) printf("WALL self test: %d case(s) wrong\n", bad);
    else     printf("WALL self test: 2 cases, all as expected\n");
    printf("\n");

    printf("STALE check self test\n");
    bad += oc_selftest_stale("accent label with no flag, fires", false, true);
    bad += oc_selftest_stale("the same label flagged, clear", true, false);
    if (bad) printf("STALE self test: %d case(s) wrong\n", bad);
    else     printf("STALE self test: 2 cases, all as expected\n");
    printf("\n");

    printf("LAYER check self test\n");
    {
        // Nothing lives on either layer in the product today, which is the same
        // standing WALL and CUT have: a clean sweep says nothing until the
        // check is shown to still fire. One resident must report, an empty
        // layer must not -- a check that fired on everything would fail the
        // second exactly as a dead one fails the first.
        lv_obj_t *scr = lv_obj_create(NULL);
        lv_screen_load(scr);
        lv_obj_t *stray = lv_label_create(lv_layer_top());
        lv_label_set_text(stray, "STRAY");
        lv_obj_set_pos(stray, 100, 100);
        lv_refr_now(NULL);

        s_findings = 0; s_seen_n = 0;
        oc_check_layer("selftest");
        int got = s_findings > 0;
        printf("  %-46s %s (%d finding%s)\n", "a label on lv_layer_top, fires",
               got ? "ok" : "FAILED", s_findings, s_findings == 1 ? "" : "s");
        bad += got ? 0 : 1;

        lv_obj_delete(stray);
        lv_refr_now(NULL);
        s_findings = 0; s_seen_n = 0;
        oc_check_layer("selftest");
        got = s_findings > 0;
        printf("  %-46s %s (%d finding%s)\n", "both layers empty, clear",
               got ? "FAILED" : "ok", s_findings, s_findings == 1 ? "" : "s");
        bad += got ? 1 : 0;

        // The exemption, asserted rather than assumed: the auto-lock warning is
        // a full screen dimmer with a card inside it, and neither may report.
        lv_obj_t *ovl = lv_obj_create(lv_layer_top());
        lv_obj_set_size(ovl, LV_PCT(100), LV_PCT(100));
        lv_obj_set_pos(ovl, 0, 0);
        lv_obj_t *inner = lv_label_create(ovl);
        lv_label_set_text(inner, "LOCKING SOON");
        lv_obj_set_pos(inner, 300, 220);
        lv_refr_now(NULL);
        s_findings = 0; s_seen_n = 0;
        oc_check_layer("selftest");
        got = s_findings > 0;
        printf("  %-46s %s (%d finding%s)\n",
               "a full screen overlay and its card, clear",
               got ? "FAILED" : "ok", s_findings, s_findings == 1 ? "" : "s");
        bad += got ? 1 : 0;
        lv_obj_delete(ovl);
        lv_refr_now(NULL);
    }
    if (bad) printf("LAYER self test: %d case(s) wrong\n", bad);
    else     printf("LAYER self test: 3 cases, all as expected\n");
    printf("\n");

    printf("ROLE check self test\n");
    wt_accent_set(WT_ACC_ORANGE);
    bad += oc_selftest_case("ORANGE accent fill + WT_STOP border, fires",
                            WT_ACC_ORANGE, wt_accent(), stop, true);
    wt_accent_set(WT_ACC_GREEN);
    bad += oc_selftest_case("GREEN accent fill + WT_OK border, one colour",
                            WT_ACC_GREEN, wt_accent(), ok, false);
    bad += oc_selftest_case("MONO ink fill + WT_STOP border, no accent",
                            WT_ACC_MONO, ink, stop, false);
    bad += oc_selftest_case("ORANGE, no status colour anywhere",
                            WT_ACC_ORANGE, key, ink, false);

    wt_accent_set(WT_ACC_MONO);
    if (bad) printf("ROLE self test: %d case(s) wrong\n", bad);
    else     printf("ROLE self test: 4 cases, all as expected\n");
    return bad ? 1 : 0;
}


// ---------------------------------------------------------------- LAYER
// What every other check in this file cannot see.
//
// All nine of them walk lv_screen_active(). LVGL draws two more layers ABOVE
// it -- lv_layer_top() and lv_layer_sys() -- and an object parented there is
// painted over every screen there is while being invisible to a tree walk that
// starts at the screen. So a stray one is not merely missed: it is missed by
// TEXT, by CONTENT, by CLIPPED and by the six others at once, on every stop,
// forever.
//
// That is not hypothetical. The home screen's unlock hand off -- the
// fingerprint that decrypts centre screen and glides into the corner chip --
// lived on lv_layer_top() for its whole life, so a flight still in the air when
// anything opened over the home kept painting the fingerprint across it. The
// walk had been shipping the proof the entire time: sim_setup_method.ppm was
// NEW SEED WORDS with an 800px "12A4BB6B" lying over the first row, one screen
// after the wipe that erased the wallet it names, and every gate called it
// clean because no gate was looking at that layer.
//
// The rule this enforces is that a settled stop has nothing up there EXCEPT a
// full screen overlay. That exemption is the auto-lock warning (main.c), and it
// is written as a shape rather than as a stop name on purpose: the toast is
// driven by a clock, so the stop it lands on can move, and the thing that makes
// it legitimate is precisely that it is a deliberate dimmer covering the whole
// glass. Nothing stranded is ever 800x480 -- a leftover label, card, action or
// chip is a fragment of a screen and reports.
static void oc_layer_walk(lv_obj_t *o, const char *tag, const char *layer,
                          int depth)
{
    if (depth && oc_visible(o)) {
        lv_area_t c;
        lv_obj_get_coords(o, &c);
        // The deliberate full screen overlay, and everything it contains: an
        // element that outranks every screen is what the top layer is FOR.
        if (c.x1 <= 0 && c.y1 <= 0 &&
            area_w(&c) >= LV_HOR_RES && area_h(&c) >= LV_VER_RES) return;
        if (area_w(&c) > 0 && area_h(&c) > 0) {
            char t[64], sig[192], detail[320];
            oc_text(o, t, sizeof t);
            snprintf(sig, sizeof sig, "LAYER|%s|%s", layer, t);
            snprintf(detail, sizeof detail,
                     "LAYER    %s holds \"%s\" at %d,%d %dx%d -- it draws over "
                     "EVERY screen and no other check in this file can see it; "
                     "parent it to the screen that owns it",
                     layer, t, (int)c.x1, (int)c.y1, area_w(&c), area_h(&c));
            oc_report_one(tag, sig, detail);
            return;                  // one finding per resident, not per child
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
        oc_layer_walk(lv_obj_get_child(o, i), tag, layer, depth + 1);
}

static void oc_check_layer(const char *tag)
{
    oc_layer_walk(lv_layer_top(), tag, "lv_layer_top", 0);
    oc_layer_walk(lv_layer_sys(), tag, "lv_layer_sys", 0);
}

// LAYER exempts the deliberate full screen overlay, and that exemption used to
// cover everything INSIDE it as well -- so the auto-lock banner, the one thing
// the top layer legitimately holds, was the one thing no check in this file
// ever read. It shipped clipped at both ends for the life of the feature.
//
// The exemption stays: an overlay covering the glass is what the layer is for,
// and running TEXT or CONTENT across it would report the covering itself. What
// runs is the clip pair, against the overlay's own tree, which is exactly the
// question an overlay can get wrong.
static void oc_check_overlay(const char *tag)
{
    lv_obj_t *top = lv_layer_top();
    const lv_area_t full = { 0, 0, LV_HOR_RES - 1, LV_VER_RES - 1 };

    for (uint32_t i = 0; i < lv_obj_get_child_count(top); i++) {
        lv_obj_t *o = lv_obj_get_child(top, i);
        if (!oc_visible(o)) continue;
        lv_area_t c;
        lv_obj_get_coords(o, &c);
        if (c.x1 > 0 || c.y1 > 0 ||
            area_w(&c) < LV_HOR_RES || area_h(&c) < LV_VER_RES) continue;
        s_n = 0;
        oc_collect(o, full, false);
        oc_check_clipped(tag);
        s_n = 0;
    }
}

// ---------------------------------------------------------------- entry points

void oc_check(const char *tag);
int  oc_report(void);

// ---- what is held BETWEEN stops -------------------------------------------
//
// The end-of-run watermark is 107728 and the worst STOP holds 76464. Thirty
// one kilobytes are live at a moment no stop photographs, so nothing that
// walks a settled screen can see them -- including every check in this file.
//
// This samples on LVGL's own clock instead. At each new high it records what
// is REACHABLE from the three roots, which is the question that separates the
// two candidates: a screen that genuinely builds that much during a
// transition, or objects nobody can reach because lv_obj_delete_async has
// queued them and lv_timer_handler has not run the queue yet. This tree calls
// delete_async 76 times.
static uint32_t s_peak_used, s_peak_reach, s_peak_scr, s_peak_top;
static const char *s_peak_tag = "(before the first stop)";
static const char *s_last_tag = "(before the first stop)";

static uint32_t oc_count(lv_obj_t *o)
{
    uint32_t n = 1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
        n += oc_count(lv_obj_get_child(o, i));
    return n;
}

static void oc_heap_sample(lv_timer_t *t);
// The construction moment: wt_screen is about to build `title`, and whatever
// it is replacing has not been freed yet.
static void oc_heap_screen(const char *title)
{
    s_last_tag = title ? title : "(untitled screen)";
    oc_heap_sample(NULL);
}
static void oc_heap_ev(lv_event_t *e) { (void)e; oc_heap_sample(NULL); }

static void oc_heap_sample(lv_timer_t *t)
{
    (void)t;
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    const uint32_t used = m.total_size - m.free_size;
    if (used <= s_peak_used) return;
    s_peak_used = used;
    s_peak_tag  = s_last_tag;
    lv_obj_t *scr = lv_screen_active();
    s_peak_scr = scr ? oc_count(scr) : 0;
    s_peak_top = oc_count(lv_layer_top()) + oc_count(lv_layer_sys());
    s_peak_reach = s_peak_scr + s_peak_top;
}

void oc_check(const char *tag)
{
    lv_obj_t *scr = lv_screen_active();
    if (!scr) return;
    s_last_tag = oc_short_tag(tag);
    if (getenv("OVERLAPCHECK_HEAP")) {
        static lv_timer_t *hs;
        if (!hs) {
            hs = lv_timer_create(oc_heap_sample, 1, NULL);
            // A 1ms timer only samples BETWEEN handler passes, and it topped
            // out 30KB under the watermark: the peak is inside a pass, while
            // a screen is being built. The display's own refresh events are
            // in that pass.
            lv_display_t *d = lv_display_get_default();
            lv_display_add_event_cb(d, oc_heap_ev, LV_EVENT_REFR_START, NULL);
            lv_display_add_event_cb(d, oc_heap_ev, LV_EVENT_REFR_READY, NULL);
            wt_screen_set_sink(oc_heap_screen);
        }
    }

    if (oc_is_game_frame(tag)) { s_skipped++; return; }

    s_stops++;
    s_n = 0;

    lv_area_t full = { 0, 0, LV_HOR_RES - 1, LV_VER_RES - 1 };
    oc_collect(scr, full, false);
    oc_mark_buried();

    if (getenv("OVERLAPCHECK_DEBUG")) {
        int b = 0;
        for (int i = 0; i < s_n; i++) if (s_node[i].buried) b++;
        printf("[dbg] %-28s %3d nodes, %3d buried, action row %d\n",
               oc_short_tag(tag), s_n, b, (int)oc_has_action_row());
    }
    const char *focus = getenv("OVERLAPCHECK_FOCUS");
    if (focus && strstr(oc_short_tag(tag), focus)) {
        for (int i = 0; i < s_n; i++) {
            if (s_node[i].vis.y2 < 340 || s_node[i].buried) continue;
            char t[64]; oc_text(s_node[i].obj, t, sizeof t);
            lv_area_t c; lv_obj_get_coords(s_node[i].obj, &c);
            printf("[foc] %-42s lbl%d scrl%d ovf%d cut%d  coords y %d..%d  vis y %d..%d  x %d..%d\n",
                   t, (int)s_node[i].is_label,
                   (int)lv_obj_has_flag(s_node[i].obj, LV_OBJ_FLAG_SCROLLABLE),
                   (int)lv_obj_has_flag(s_node[i].obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE),
                   (int)s_node[i].cut, (int)c.y1, (int)c.y2,
                   (int)s_node[i].vis.y1, (int)s_node[i].vis.y2,
                   (int)s_node[i].vis.x1, (int)s_node[i].vis.x2);
        }
    }

    // OVERLAPCHECK_SIZES=1: every rendered label with its font height and the
    // stop it is on. Not a check -- the answer to "show me everything small",
    // which the checks cannot give because each one is defined by what it
    // excuses. Piped through sort/uniq it is the whole device's type ladder.
    if (getenv("OVERLAPCHECK_SIZES")) {
        for (int i = 0; i < s_n; i++) {
            const oc_node_t *n = &s_node[i];
            if (!n->is_label || n->buried) continue;
            char spbuf[512];
        const char *txt = oc_text_of(n->obj, spbuf, sizeof spbuf);
            if (!txt || !*txt) continue;
            char t[96];
            oc_text(n->obj, t, sizeof t);
            const lv_font_t *f = lv_obj_get_style_text_font(n->obj, LV_PART_MAIN);
            const char *fn = f == wt_font14()      ? "font14"
                           : f == wt_font_mono14() ? "mono14"
                           : f == wt_font_mono18() ? "mono18"
                           : f == wt_font_mono21() ? "mono21"
                           : f == wt_font23()      ? "font23"
                           : f == wt_font_mono23() ? "mono23"
                           : f == wt_font28()      ? "font28"
                           : f == wt_font_mono28() ? "mono28"
                           : "other";
            printf("[size] %2d %-6s %4d %4d %p %-30s %s\n", n->lh, fn,
                   (int)n->vis.x1, (int)n->vis.y1, (void *)n->parent,
                   oc_short_tag(tag), t);
        }
    }

    // OVERLAPCHECK_HEAP=1: what this stop is HOLDING, per stop. The
    // end-of-run [lvheap] line is a global high-water mark and names no
    // screen, so a tree at 87% says nothing about WHICH page to cut. Sorted,
    // this is that list.
    if (getenv("OVERLAPCHECK_HEAP")) {
        lv_mem_monitor_t m;
        lv_mem_monitor(&m);
        printf("[heap] %7u %s\n",
               (unsigned)(m.total_size - m.free_size), oc_short_tag(tag));
    }

    oc_check_text_overlap(tag);
    oc_check_content_bottom(tag);
    oc_check_ladder(tag);
    oc_check_wrap_growth(tag);
    oc_check_clipped(tag);
    oc_check_colour_roles(tag);
    oc_check_stale(tag);
    oc_check_bare(tag);
    oc_check_wall(tag);
    oc_check_exit(tag);
    oc_check_void(tag);
    oc_check_fit(tag);
    oc_check_cut(tag);
    oc_check_ink(tag);
    oc_check_tiny(tag);
    oc_check_dots(tag);
    oc_check_amber(tag);
    oc_check_ragged(tag);
    oc_check_layer(tag);
    // LAST: it rebuilds the node set against the overlay's tree, so anything
    // reading the screen's set has to have read it already.
    oc_check_overlay(tag);
}

int oc_report(void)
{
    const char *lang = getenv("SIM_LANG");
    if (!lang || !*lang) lang = "en";

    printf("\n[overlap] %s: %d stops checked, %d game frames skipped, "
           "%d distinct findings\n", lang, s_stops, s_skipped, s_findings);
    if (s_peak_used)
        printf("[heap-peak] %u bytes, %u objects reachable "
               "(%u on the screen, %u on the layers), just after %s\n",
               (unsigned)s_peak_used, (unsigned)s_peak_reach,
               (unsigned)s_peak_scr, (unsigned)s_peak_top, s_peak_tag);

    // Where the walk STARTED, not wt_accent_name(). Two reasons: the walk taps
    // the theme dots near the end and leaves on MONO, so the live theme would
    // label every run MONO; and those same taps are why a MONO run still
    // reports a couple of dozen accent objects rather than none.
    const char *acc = getenv("SIM_ACCENT");
    if (s_role_accent_objs || s_role_status_objs)
        printf("[overlap] %s: role check saw %d accent and %d status objects, "
               "walk started in %s\n", lang, s_role_accent_objs,
               s_role_status_objs, acc && *acc ? acc : "MONO");

    // An entry that was never hit means its screen has been rebuilt (or renamed)
    // and the exemption is now protecting nothing. Printed rather than failed,
    // because the same list is read by every locale's run and a screen only
    // reachable in some walks would otherwise turn a fix into a build break.
    {
        int bare_left = 0;
        for (unsigned i = 0; i < sizeof OC_BARE_BACKLOG / sizeof OC_BARE_BACKLOG[0]; i++) {
            if (!OC_BARE_BACKLOG[i]) continue;
            if (s_bare_hit[i]) { bare_left++; continue; }
            printf("[overlap] %s: BARE backlog entry \"%s\" never matched a stop"
                   " -- rebuild it or delete the line\n", lang, OC_BARE_BACKLOG[i]);
        }
        printf("[overlap] %s: %d screens still on the BARE backlog\n",
               lang, bare_left);
    }
    {
        int wall_left = 0;
        for (unsigned i = 0; i < sizeof OC_WALL_BACKLOG / sizeof OC_WALL_BACKLOG[0]; i++) {
            if (!OC_WALL_BACKLOG[i]) continue;
            if (s_wall_hit[i]) { wall_left++; continue; }
            printf("[overlap] %s: WALL backlog entry \"%s\" never matched a stop"
                   " -- rebuild it or delete the line\n", lang, OC_WALL_BACKLOG[i]);
        }
        printf("[overlap] %s: %d screens still on the WALL backlog\n",
               lang, wall_left);
    }
    {
        int void_left = 0;
        for (unsigned i = 0; i < sizeof OC_VOID_BACKLOG / sizeof OC_VOID_BACKLOG[0]; i++) {
            if (!OC_VOID_BACKLOG[i]) continue;
            if (s_void_hit[i]) { void_left++; continue; }
            printf("[overlap] %s: VOID backlog entry \"%s\" never matched a stop"
                   " -- rebuild it or delete the line\n", lang, OC_VOID_BACKLOG[i]);
        }
        printf("[overlap] %s: %d screens still on the VOID backlog\n",
               lang, void_left);
    }
    {
        int exit_left = 0;
        for (unsigned i = 0; i < sizeof OC_EXIT_BACKLOG / sizeof OC_EXIT_BACKLOG[0]; i++) {
            if (!OC_EXIT_BACKLOG[i]) continue;
            if (s_exit_hit[i]) { exit_left++; continue; }
            printf("[overlap] %s: EXIT backlog entry \"%s\" never matched a stop"
                   " -- rebuild it or delete the line\n", lang, OC_EXIT_BACKLOG[i]);
        }
        printf("[overlap] %s: %d screens still on the EXIT backlog\n",
               lang, exit_left);
    }
    {
        int fit_left = 0;
        for (unsigned i = 0; i < sizeof OC_FIT_BACKLOG / sizeof OC_FIT_BACKLOG[0]; i++) {
            if (!OC_FIT_BACKLOG[i]) continue;
            if (s_fit_hit[i]) { fit_left++; continue; }
            printf("[overlap] %s: FIT backlog entry \"%s\" never matched a stop"
                   " -- cut it from the list, the string it excused is gone\n",
                   lang, OC_FIT_BACKLOG[i]);
        }
        printf("[overlap] %s: %d strings still on the FIT backlog\n",
               lang, fit_left);
    }
    {
        // STALE is the one backlog whose verdict is NOT this run's to give.
        // It needs the accent to CHANGE, only the accent sweep changes it, and
        // it is blind in the MONO run whose accent is WT_INK -- so an entry
        // unmatched here may match in another pass, and "never matched a stop"
        // would be a lie two runs out of three. Report what THIS run saw and
        // let run_overlapcheck.sh decide across all three.
        //
        // Written because s_stale_hit was set and never read: every other
        // backlog in this file says when an entry stopped excusing anything,
        // and this one silently kept it forever. That is how a list of
        // excuses outlives the defects it was written for.
        for (unsigned i = 0; i < sizeof OC_STALE_BACKLOG / sizeof OC_STALE_BACKLOG[0]; i++) {
            if (!OC_STALE_BACKLOG[i]) continue;
            printf("[overlap] %s: STALE backlog entry %s |%s|\n",
                   lang, s_stale_hit[i] ? "matched" : "unmatched",
                   OC_STALE_BACKLOG[i]);
        }
    }

    for (int i = 0; i < s_seen_n; i++)
        if (s_seen_hits[i] > 1)
            printf("[overlap] %s: seen at %d stops: %s\n",
                   lang, s_seen_hits[i], s_seen[i]);

    if (s_seen_n >= OC_MAX_SEEN)
        printf("[overlap] %s: finding table full at %d, some were not recorded\n",
               lang, OC_MAX_SEEN);

    if (s_findings == 0) return 0;
    if (getenv("OVERLAPCHECK_STRICT")) return 1;
    printf("[overlap] %s: not failing the build, OVERLAPCHECK_STRICT is unset\n", lang);
    return 0;
}
