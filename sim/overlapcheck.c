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
//
// The first three are the ones the review asked for. The fourth was added after
// reading docs/media/sign-verify.png: a label can ask for a box taller than the
// container holding it, get clipped, and leave a row of glyphs sliced through
// the middle while the other three checks see a perfectly clean screen. Same
// root cause, absolute y under content that grows, so it belongs to this gate.
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
}

int oc_report(void)
{
    const char *lang = getenv("SIM_LANG");
    if (!lang || !*lang) lang = "en";

    printf("\n[overlap] %s: %d stops checked, %d game frames skipped, "
           "%d distinct findings\n", lang, s_stops, s_skipped, s_findings);

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
