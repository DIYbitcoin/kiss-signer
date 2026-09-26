// The touch cache: one level, one edge queue, one reader that replays it.
//
// Integer state only, no LVGL and no FreeRTOS, and that is the whole reason it
// is a header of its own. The device half of main/kiss_touch.c is a sampler
// task nothing off glass can run; everything a reader's answer actually depends
// on -- how a missed contact is counted, which point it is replayed at, when it
// stops being an input -- is in here, so the desktop simulators compile the
// SAME state machine the board runs instead of a model of it. A model drifts,
// and a gate over a model certifies a path the device does not take.
//
// The caller serialises. On the device the sampler task feeds it and the LVGL
// task reads it, so main/kiss_touch.c wraps every call below in one spinlock;
// in the simulators there is a single thread and nothing to wrap.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// A reader further behind than this has stopped being late and started being
// wrong: replaying taps after the hand has left the glass reads as the panel
// firing on its own. Two contacts is the double tap, which is the point of all
// of this; older ones than that are dropped.
#define KISS_TOUCH_BACKLOG 2

// ...and a backlog no older than this, by the wall clock.
//
// Counting contacts bounds how MANY a reader owes itself; nothing in a count
// bounds how OLD they are, and age is what turns a replay into a phantom. The
// screen that proved it is the firmware update: kiss_fw_ui.c writes the image
// on the LVGL task with the panel blacked, so neither the collector nor the
// indev is read for seconds, and an owner poking a device that has gone dark
// would have had their pokes delivered to the RESTART button the result screen
// draws when the light comes back.
//
// 250 ms, which is two numbers at once. It has to be comfortably LONGER than
// the slowest pass the collector can be called on -- a full 4.3in repaint is
// ~130 ms, so a pair of taps that landed inside one pass is never dropped for
// the reader being slow -- and far SHORTER than CW_QT_MS's 800 ms, so which
// taps make a pair stays kiss_coverword.c's judgement and not this file's. A
// human double tap's gap is 60 to 150 ms, so a whole pair fits inside the cap
// with room over.
#define KISS_TOUCH_STALE_MS 250

// One DEBOUNCED sample in: whether a finger is on the glass, and where. The
// debounce belongs to the sampler, not here -- a scripted `touch()` has nothing
// to debounce, and feeding it through a filter would only make the simulator
// disagree with itself about when a tap happened.
//
// Calling it again with the same state and a new point is how a drag is fed: it
// moves the live contact, and never counts a second one.
void kiss_touch_post(bool down, int x, int y, uint32_t now_ms);

// The live level, for a reader that wants exactly that. LVGL's pointer indev is
// the one: it turns consecutive levels into its own presses, clicks and
// gestures, so an edge replayed into it is a click at a place and a moment
// nothing was touched.
bool kiss_touch_level(int *x, int *y);

// The next thing the gesture collector has not been shown: one edge per call,
// press before lift, each at the point its own contact had. It is the only
// reader that classifies taps itself, so it is the only one that needs the
// edges a slow pass slept through.
bool kiss_touch_edge(int *x, int *y, uint32_t now_ms);
