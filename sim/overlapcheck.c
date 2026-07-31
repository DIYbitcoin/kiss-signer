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
#include "wallet_theme.h"
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
static void oc_text(lv_obj_t *o, char *out, size_t out_len)
{
    const char *t = lv_obj_check_type(o, &lv_label_class) ? lv_label_get_text(o) : NULL;
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

    lv_area_t coords;
    lv_obj_get_coords(o, &coords);

    lv_area_t vis;
    if (!oc_intersect(&vis, &coords, &clip)) return;   // clipped out entirely

    bool is_label = lv_obj_check_type(o, &lv_label_class);

    // A label with no text has a box but nothing in it, and comparing empty
    // boxes invents findings nobody can act on.
    if (is_label) {
        const char *t = lv_label_get_text(o);
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
    n->wraps     = is_label && lv_label_get_long_mode(o) == LV_LABEL_LONG_MODE_WRAP;
    n->leaf      = kids == 0;
    n->clickable = lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE);
    n->buried    = false;
    // Losing the bottom of a label to a container that scrolls is a list, and
    // the reader can bring the rest into view. Losing it to one that does not
    // is text nobody can ever read.
    n->cut       = is_label && coords.y2 > vis.y2 && !clip_scrolls;
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
        if (n->vis.y1 >= WT_CONTENT_BOTTOM && n->vis.y2 < LV_VER_RES) return true;
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

static void oc_check_content_bottom(const char *tag)
{
    char t[64], sig[192], detail[320];

    if (!oc_has_action_row()) return;

    for (int i = 0; i < s_n; i++) {
        oc_node_t *n = &s_node[i];
        if (n->buried) continue;
        // A container spans its children by definition and a full screen
        // backdrop crosses every horizontal line, so neither says anything
        // about where content was put.
        if (!n->is_label && !n->leaf) continue;
        if (area_is_backdrop(&n->vis)) continue;

        // Starts above the line and finishes at or below it: the definition of
        // reaching into the action row. Something that starts below the line is
        // already in the action row, which is where buttons belong.
        if (n->vis.y1 >= WT_CONTENT_BOTTOM) continue;
        if (n->vis.y2 < WT_CONTENT_BOTTOM) continue;

        oc_text(n->obj, t, sizeof t);
        snprintf(sig, sizeof sig, "CONTENT|%s|%d", t, (int)n->vis.y2);
        snprintf(detail, sizeof detail,
                 "CONTENT  \"%s\" runs y %d..%d, past WT_CONTENT_BOTTOM %d by %d px",
                 t, (int)n->vis.y1, (int)n->vis.y2, WT_CONTENT_BOTTOM,
                 (int)n->vis.y2 - WT_CONTENT_BOTTOM + 1);
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
// The product has a kit for this -- wt_card, wt_value_card, wt_why_block,
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
// anything with a border and a fill big enough to be a card or a chip, or a
// why-block's rule bar -- narrow, tall, and the one thing on a bare screen that
// is never present. A screen with a wall and no frame is the shape being
// rejected.
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
    // A PILL IS NOT CHROME. wt_pill draws a bordered, filled box well over the
    // size floor below, so counting buttons made every screen in the product
    // look furnished and the first run of this check found nothing at all. The
    // question is what the screen puts ABOVE the action row to carry its
    // content, so the action row itself is not an answer to it, and neither is
    // anything else the reader can press.
    if (n->clickable) return false;
    if (n->vis.y2 >= WT_CONTENT_BOTTOM) return false;
    // a why-block's coloured rule: 3px wide, as tall as the claim beside it
    if (w <= 4 && h >= 30) return true;
    if (w < 100 || h < 30) return false;
    return lv_obj_get_style_border_width(n->obj, LV_PART_MAIN) >= 1 &&
           lv_obj_get_style_bg_opa(n->obj, LV_PART_MAIN) >= LV_OPA_50;
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
    // landed; all nine have been rebuilt with wt_why_body, so every screen on
    // the device that has an action row now puts something framed above it.
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
             "element (wt_card / wt_value_card / wt_why_block / wt_chip)",
             t, (int)(wall->vis.x2 - wall->vis.x1 + 1),
             (int)(wall->vis.y2 - wall->vis.y1 + 1));
    oc_report_one(tag, sig, detail);
}

static void oc_check_colour_roles(const char *tag)
{
    char t[64], sig[192], detail[320];

    lv_color_t ac = wt_accent();
    uint32_t ahex = ((uint32_t)ac.red << 16) | ((uint32_t)ac.green << 8) | ac.blue;
    lv_color_t ink = WT_INK;
    uint32_t inkhex = ((uint32_t)ink.red << 16) | ((uint32_t)ink.green << 8) | ink.blue;
    if (cde_same(ahex, inkhex)) return;                 // MONO, see above

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

int oc_selftest(void)
{
    lv_color_t stop = WT_STOP, ok = WT_OK, ink = WT_INK, key = WT_KEY;
    int bad = 0;

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

// ---------------------------------------------------------------- entry points

void oc_check(const char *tag);
int  oc_report(void);

void oc_check(const char *tag)
{
    lv_obj_t *scr = lv_screen_active();
    if (!scr) return;

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

    oc_check_text_overlap(tag);
    oc_check_content_bottom(tag);
    oc_check_wrap_growth(tag);
    oc_check_clipped(tag);
    oc_check_colour_roles(tag);
    oc_check_bare(tag);
}

int oc_report(void)
{
    const char *lang = getenv("SIM_LANG");
    if (!lang || !*lang) lang = "en";

    printf("\n[overlap] %s: %d stops checked, %d game frames skipped, "
           "%d distinct findings\n", lang, s_stops, s_skipped, s_findings);

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
