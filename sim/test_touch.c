// Desktop tests for the touch cache -- main/kiss_touch.c's three calls.
//
// This suite exists for the same reason test_coverword.c does. The cache is
// what turned a double tap the panel was too slow to see into two taps the
// collector can count, and the first version of it was written with its whole
// risk surface on the far side of the platform seam: a sampler task, a
// controller, a board. Nothing on the host ran a line of it, every gate was
// green, and three of its rules were wrong -- a reader replayed contacts from
// before it existed, a replayed tap arrived at the NEXT tap's coordinates, and
// a backlog could sit through a firmware write and land on the buttons of the
// screen that came back.
//
// So the rules live in plain integer C with no LVGL, no FreeRTOS and no board
// in them, and this drives them directly: the clock is a number this file
// passes in, and every read is checked for its state AND its point. What is
// left on the other side of the seam is the sampler's own two jobs -- polling
// the controller at 100 Hz and debouncing the lift -- and those stay a hardware
// verdict.
//
// ONE LONG SEQUENCE, no reset between cases, and that is deliberate. The cache
// has exactly one boot on a device and nothing anywhere resets it, so a test
// seam to rewind it would be a shape the product does not have. Every case
// below therefore ends with the reader caught up, which is a clean start for
// the next one, and the FIRST case is the one that can only be asked once.
#include <stdio.h>

#include "kiss_touch.h"

static int tfails;
static uint32_t t_ms;        // the test's own clock

static void tchk(const char *name, int ok) {
    if (ok) printf("PASS: %s\n", name);
    else { printf("FAIL: %s\n", name); tfails++; }
}

static void down_at(int x, int y) { kiss_touch_post(true, x, y, t_ms); }
static void lift(void) { kiss_touch_post(false, 0, 0, t_ms); }
// A whole contact with nothing reading in between: the tap the panel was too
// busy to show anybody.
static void tap_at(int x, int y) { down_at(x, y); lift(); }

// One read, checked for what it says and where it says it. A released read is
// not checked for a point: nothing is asked to write one.
static void want(const char *name, bool press, int x, int y) {
    int rx = -1, ry = -1;
    bool got = kiss_touch_edge(&rx, &ry, t_ms);
    if (got != press) {
        printf("FAIL: %s: read %s, wanted %s\n", name, got ? "pressed" : "released",
               press ? "pressed" : "released");
        tfails++;
        return;
    }
    if (press && (rx != x || ry != y)) {
        printf("FAIL: %s: pressed at %d,%d, wanted %d,%d\n", name, rx, ry, x, y);
        tfails++;
        return;
    }
    printf("PASS: %s\n", name);
}

int test_touch(void) {
    t_ms = 5000;   // not zero: a cache whose rules rest on tick 0 would flatter itself

    // ---- a reader's first read is the PRESENT -------------------------------
    // Contacts counted before a reader existed happened to a screen it was not
    // there for. This is the only case that can be asked once, so it is first.
    tap_at(10, 10);
    tap_at(20, 20);
    want("first read replays nothing from before it", false, 0, 0);
    want("...and nothing behind that either", false, 0, 0);

    // ---- a contact the reader keeps up with ---------------------------------
    // The ordinary case, and the one every walk stop drives: the level, at the
    // live point, tracking the finger.
    t_ms += 16;
    down_at(100, 100);
    want("a press is the live point", true, 100, 100);
    down_at(140, 120);                     // the finger moved: still one contact
    want("a drag follows the finger", true, 140, 120);
    lift();
    want("the lift comes through", false, 0, 0);
    want("and stays up", false, 0, 0);

    // ---- a whole contact between two reads ---------------------------------
    // The reported bug: a repaint long enough to hide the lift. The contact is
    // replayed, press then lift, at the point it was made -- not at wherever
    // the glass is now, which is nowhere.
    t_ms += 130;                           // one full 4.3in repaint
    tap_at(200, 60);
    want("a tap nobody saw is replayed", true, 200, 60);
    want("...and then its lift", false, 0, 0);
    want("...and nothing more", false, 0, 0);

    // ---- TWO contacts in one gap, at different places ----------------------
    // Each arrives where it was made. Serving the live point instead put both
    // of them at the second one's coordinates, which is the corner pair lost
    // and a round of the game started in its place.
    t_ms += 130;
    tap_at(300, 80);
    tap_at(40, 40);
    want("the first of two keeps its own point", true, 300, 80);
    want("...its lift", false, 0, 0);
    want("the second keeps its own point", true, 40, 40);
    want("...its lift", false, 0, 0);
    want("...and the pair is done", false, 0, 0);

    // ---- deeper than the cache will hold -----------------------------------
    // The NEWEST KISS_TOUCH_BACKLOG contacts are kept. Jumping straight to the
    // present would lose the pair, which is the one input this exists for.
    t_ms += 130;
    tap_at(1, 1);
    tap_at(2, 2);
    tap_at(3, 3);
    want("three taps deep, the oldest is dropped", true, 2, 2);
    want("...its lift", false, 0, 0);
    want("...the newest is kept", true, 3, 3);
    want("...its lift", false, 0, 0);
    want("...and no fourth edge", false, 0, 0);

    // ---- the trim while a contact is being held -----------------------------
    // A reader mid-contact is owed its LIFT, and the trim does not get to
    // swallow it: two presses in a row with no release between them is ONE
    // stroke to the collector, which is the merge all of this exists to end.
    t_ms += 16;
    down_at(50, 50);
    want("a held contact", true, 50, 50);
    lift();
    tap_at(60, 60);
    tap_at(70, 70);
    tap_at(80, 80);
    want("the trim pays the held contact's lift first", false, 0, 0);
    want("...then the newest two", true, 70, 70);
    want("...its lift", false, 0, 0);
    want("...the last one", true, 80, 80);
    want("...its lift", false, 0, 0);
    want("...and caught up", false, 0, 0);

    // ---- a backlog older than the cap is not an input ----------------------
    // The firmware write: the panel is black, nothing reads the seam for
    // seconds, and an owner prods it. Those prods must not land on the buttons
    // of the screen that comes back.
    t_ms += 16;
    tap_at(400, 200);
    t_ms += KISS_TOUCH_STALE_MS + 100;
    want("a tap slept through is dropped", false, 0, 0);
    want("...and nothing follows it", false, 0, 0);

    // ---- ...and one inside the cap still is ---------------------------------
    // Same input, a shorter sleep. Without this the case above would pass on a
    // cache that had simply stopped working.
    tap_at(410, 210);
    t_ms += KISS_TOUCH_STALE_MS - 50;
    want("a fresh tap survives a slow pass", true, 410, 210);
    want("...its lift", false, 0, 0);
    want("...and no more", false, 0, 0);

    // ---- a replay already running is not cut short by the clock ------------
    // One edge leaves per pass, so on the 4.3in the second tap of a pair is not
    // due until three repaints after the finger made it -- past the cap. The
    // cap judges a backlog ONCE, as it appears; ageing each edge as it came up
    // for delivery would drop exactly the second tap of the pair.
    t_ms += 16;
    tap_at(7, 8);
    tap_at(9, 10);
    t_ms += 130;
    want("a slow pass gets the first tap", true, 7, 8);
    t_ms += 130;
    want("...its lift", false, 0, 0);
    t_ms += 130;                            // now 390 ms after the contact ended
    want("...and the second tap is still delivered", true, 9, 10);
    t_ms += 130;
    want("...its lift", false, 0, 0);

    // ---- the level, which is what the indev is shown -----------------------
    int lx = -1, ly = -1;
    tchk("nothing on the glass is no level", !kiss_touch_level(&lx, &ly));
    t_ms += 16;
    down_at(11, 22);
    tchk("a finger is a level at its point",
         kiss_touch_level(&lx, &ly) && lx == 11 && ly == 22);
    want("the collector sees the same contact", true, 11, 22);
    lift();
    tchk("a lift ends the level", !kiss_touch_level(&lx, &ly));
    want("...and the collector's lift", false, 0, 0);

    return tfails;
}
