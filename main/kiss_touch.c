// The touch controller's ONE consumer, on a clock of its own.
//
// Both boards' controllers answer "is there a finger" as a ONE SHOT, and both
// of the vendored drivers say so in their own way. The GT911 raises a
// buffer-ready flag that a read clears -- and esp_lcd_touch_gt911_read_data
// clears it even when the flag was not set, without invalidating the points it
// already holds, so a read that finds no new frame hands back the PREVIOUS
// state. The FT5x06's get_xy zeroes tp->data.points as it hands them out, so
// each point is delivered exactly once. Read a thing like that from two places
// and each read hides an edge from the other; read it on the repaint's clock
// and every edge between two repaints is simply gone.
//
// Both were happening, and together they were the cover screen's bug. main.c's
// game_tick polls the seam once per LVGL pass and runs its own gesture
// collector; kiss_ui.c's pointer indev polls the same handle again, from the
// moment the first signer screen creates it. An LVGL pass on the cover is a
// REPAINT -- the fruit animate continuously and a full one is ~130 ms of
// scalar rendering on the 4.3in, not the 16 ms the timer asks for. A human
// double tap's lift is shorter than that, so the two taps arrived as one
// unbroken press: one tap, which only ARMS the corner pair (kiss_coverword.c),
// and the corner branch suppresses start_game(), so from a cold boot the
// shortcut did nothing whatsoever. After one unlock the indev's second drain
// per pass started catching the lift by accident, which is exactly why the
// owner had to draw the word first.
//
// So there is one consumer, here, sampling at SAMPLE_MS, and every reader is
// served from what it cached. No reader touches I2C any more.
//
// AND THE EDGES ARE HELD FOR ONE READER. Sampling faster does not by itself
// give a slow reader back the lift it slept through: the cache would still say
// "pressed" on both sides of it. So the cache counts contacts, and the gesture
// collector -- the reader that classifies taps itself -- is handed the next
// edge it has not been shown, one per call, press before lift. A double tap
// that lands entirely inside one repaint reaches the collector as press, lift,
// press, lift over four passes instead of as one press. That is the fix; the
// 100 Hz is what makes the edges exist to be counted.
//
// ONE replay reader, and not one per reader. The other reader is LVGL's pointer
// indev, and an indev does not want edges: it is handed a LEVEL every pass and
// derives its own press, click and gesture from the sequence, so a replayed
// press is a click on whatever is under a coordinate the finger has already
// left. It gets kiss_touch_level() and the merging this file exists to stop
// cannot reach it -- the indev never classified a double tap in the first
// place, the cover screen's collector did. Two seats was the shape this file
// started in, and it bought nothing: it put phantom presses on the passphrase
// keyboard for a reader that had no use for an edge.
//
// The bookkeeping itself is in kiss_touch.h's three calls, which the simulators
// compile too, so the desktop gates walk the same state machine.
#include "kiss_touch.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kiss_board.h"

static const char *TAG = "kiss";

// 100 Hz. Both controllers report at about that rate themselves, so this reads
// each frame about once; faster only spends I2C, and slower starts losing the
// frames this file exists to keep.
#define SAMPLE_MS 10

// A lift is believed after this many consecutive empty samples, so 40 ms of
// confirmed absence.
//
// The number is against two things. Skin loses contact for a controller frame
// or two under a light finger -- 10 to 20 ms at these panels' report rate --
// and until this file existed the GT911's repeat-the-last-state hid every one
// of them; now an unfiltered dropout would be a real pen lift, which splits a
// letter of the drawn unlock word (dev/decisions.md has the signer that would
// not open to its own word) and breaks a blade trail. 40 ms swallows three
// consecutive empty frames, which is more than a finger resting on glass has
// ever produced. And the shortest gap that must still come through is the one
// between two taps of a human double tap, 60 ms at the very fastest, so the
// lift is seen with 20 ms to spare at the worst case and 100 ms to spare at a
// normal one. The cost is every release arriving up to 40 ms late, which is far
// inside CW_QT_MS (800 ms for the second tap) and inside LVGL's click timing.
#define UP_SAMPLES 4
#else
// The desktop builds name their board with -DKISS_BOARD_<ID>, and the header is
// what turns that into KISS_BLADE_LANDING below. The device has it from above.
#include "kiss_board.h"
#endif

// How many recent contacts keep a point of their own. A reader can be
// KISS_TOUCH_BACKLOG contacts behind plus the one under the finger, and a power
// of two makes the index a mask on the contact number.
#define PT_N 4
#define PT_MASK (PT_N - 1)

static bool s_down;        // debounced: a finger is on the glass
static uint32_t s_downs;   // touch-down edges since boot; also the contact's number
static uint32_t s_ups;     // lift edges since boot

// A POINT PER CONTACT, and this is the second thing two seats got wrong. A
// replay that carries the LIVE point is a tap delivered where the NEXT tap
// landed: two contacts inside one pass at different places both arrived at the
// second one's coordinates, which is the corner pair lost and Fruit Island
// started instead. So each contact carries its own, kept up to date while it
// lasts -- for the contact still under the finger that is the live point, and
// for one already over it is where that contact ended, which for a tap is the
// tap.
static struct {
  int x, y;
  uint32_t ended;          // when its lift was recorded, ms; meaningless while live
#if KISS_BLADE_LANDING
  int x0, y0;              // where it came down: the game's blade starts there
#endif
} s_pt[PT_N];

// What the replaying reader has been shown. downs - ups == (held ? 1 : 0),
// always, on the way out of every call: it is handed alternating edges, so it
// can never be told of two presses without the lift between them.
static struct {
  bool seen;               // it has read at least once
  bool held;               // it is being shown a contact right now
  bool draining;           // it is part way through a backlog
  uint32_t downs, ups;
  uint32_t judged;         // the contact count the age question was last asked at
} s_seat;

void kiss_touch_post(bool down, int x, int y, uint32_t now_ms)
{
  if (down) {
    if (!s_down) {
      s_down = true;
      s_downs++;
#if KISS_BLADE_LANDING
      s_pt[s_downs & PT_MASK].x0 = x;
      s_pt[s_downs & PT_MASK].y0 = y;
#endif
    }
    s_pt[s_downs & PT_MASK].x = x;
    s_pt[s_downs & PT_MASK].y = y;
  } else if (s_down) {
    s_down = false;
    s_ups++;
    s_pt[s_downs & PT_MASK].ended = now_ms;
  }
}

bool kiss_touch_level(int *x, int *y)
{
  if (!s_down) return false;
  *x = s_pt[s_downs & PT_MASK].x;
  *y = s_pt[s_downs & PT_MASK].y;
  return true;
}

// Bring the reader to the present: it is told the level and owed nothing.
static void seat_now(void)
{
  s_seat.downs = s_downs;
  s_seat.ups = s_ups;
  s_seat.held = s_down;
  s_seat.draining = false;
  s_seat.judged = s_downs;
}

bool kiss_touch_edge(int *x, int *y, uint32_t now_ms)
{
  // A reader's first read starts at the present, not at boot. Everything
  // counted before it existed happened to a screen it was not there for, and
  // handing it over is how a freshly opened signer gets pressed by a contact
  // that belonged to the cover. game_tick has existed since build_game, so on
  // this board that is a guard rather than a live path -- but it is the
  // invariant the whole seat rests on, and it is why a reader that appears
  // later (the indev) is given the level instead of a seat of its own.
  if (!s_seat.seen) {
    s_seat.seen = true;
    seat_now();
  }

  // A BACKLOG IS JUDGED ONCE, as it appears, and then replayed to the end.
  // Ageing each edge as it comes up for delivery would drop the second tap of
  // the very pair this exists for: one edge goes out per pass, so on the 4.3in
  // the second tap's press is not due until ~3 repaints (~400 ms) after the
  // finger made it, and it would age out while queued correctly. What is asked
  // here instead is whether the newest thing the reader missed is still an
  // input at all. If the reader has been away for a screen -- the update
  // write, a long modal -- the answer is no and the whole backlog goes.
  // ASKED AGAIN EVERY TIME A CONTACT JOINS, and not once per backlog: a reader
  // one edge into a replay is already draining, so a judgement made then would
  // stand over everything that arrived during a stall that began a moment
  // later -- taps a second old replayed at full age, which is the case this
  // cap exists to refuse. Asked per contact count rather than per call, so a
  // correctly queued pair is not re-judged while it is going out.
  if (s_downs != s_seat.downs && s_downs != s_seat.judged) {
    uint32_t newest = s_down ? now_ms : s_pt[s_downs & PT_MASK].ended;
    s_seat.judged = s_downs;
    if ((uint32_t)(now_ms - newest) > KISS_TOUCH_STALE_MS) seat_now();
    else s_seat.draining = true;
  }

  if (s_downs - s_seat.downs > KISS_TOUCH_BACKLOG) {
    // The NEWEST KISS_TOUCH_BACKLOG contacts are kept and the older ones dropped,
    // rather than jumping to the present: a reader that fell three taps behind
    // and then reported nothing at all would lose the pair, which is the one
    // input this file exists for.
    s_seat.downs = s_downs - KISS_TOUCH_BACKLOG;
    // A reader in the middle of a contact is still owed its LIFT, and the trim
    // does not get to swallow that: two presses in a row with no release
    // between them is one stroke to the collector, which is the merge this
    // whole file exists to end. So it is handed the release first and the
    // replay starts from there.
    s_seat.ups = s_seat.held ? s_seat.downs - 1 : s_seat.downs;
  }

  bool pressed = false;
  if (s_seat.held) {
    if (s_seat.ups != s_ups) {
      s_seat.ups++;                    // the lift of the contact it holds
      s_seat.held = false;
    } else {
      pressed = true;
    }
  } else if (s_seat.downs != s_downs) {
    s_seat.downs++;                    // a contact it has not been shown
    s_seat.held = true;
    pressed = true;
  }
  if (pressed) {
    // Its own contact's point, which for the live one is the live point.
    *x = s_pt[s_seat.downs & PT_MASK].x;
    *y = s_pt[s_seat.downs & PT_MASK].y;
  }
  // Caught up with the glass: the next contact starts a fresh backlog and gets
  // aged on its own merits.
  if (s_seat.downs == s_downs && s_seat.held == s_down) s_seat.draining = false;
  return pressed;
}

#ifdef ESP_PLATFORM
// Written by the sampler task, read by the LVGL task. A spinlock rather than a
// mutex: the guarded region is a handful of loads and stores, the readers run
// inside an LVGL pass, and the sampler must never be made to wait on them.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_live;            // the sampler is running (app_main's task only)

static void sample_task(void *arg)
{
  (void)arg;
  TickType_t next = xTaskGetTickCount();
  int up_run = 0;
  for (;;) {
    int x = 0, y = 0;
    // Outside the critical section, always: this is an I2C transaction that
    // blocks, and the bus is shared with the camera sensor's SCCB
    // (kiss_scan_set_bus / camera_spike_set_bus). The i2c_master driver
    // serialises the bus itself, so the two cannot corrupt each other; what
    // this keeps is the sampler off the readers' backs while it waits.
    bool raw = kiss_board_touch_point(&x, &y);
    if (raw) up_run = 0;
    else if (up_run < UP_SAMPLES) up_run++;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    portENTER_CRITICAL(&s_mux);
    if (raw) kiss_touch_post(true, x, y, now);
    else if (up_run >= UP_SAMPLES) kiss_touch_post(false, 0, 0, now);
    portEXIT_CRITICAL(&s_mux);
    // A fixed cadence, not a fixed sleep: an I2C transaction that took longer
    // than usual must not push every later sample out with it.
    //
    // ...but a deadline already gone stays gone. xTaskDelayUntil does not block
    // when the time it is given is in the past -- it advances the deadline and
    // returns -- so after ONE long iteration the task would run flat out, above
    // the display loop, until it caught up. One long iteration is not
    // hypothetical: the camera's sensor register tables are written over this
    // same bus at 100 kHz when a preview starts. Being late costs the samples
    // that were missed and nothing more, and a sample that ate a whole period
    // gives up its slot as well, so this loop can never run without yielding.
    TickType_t nowt = xTaskGetTickCount();
    if ((int32_t)(nowt - next) >= (int32_t)pdMS_TO_TICKS(SAMPLE_MS)) next = nowt;
    (void)xTaskDelayUntil(&next, pdMS_TO_TICKS(SAMPLE_MS));
  }
}

void kiss_touch_start(void)
{
  // A controller that never answered leaves this alone: kiss_board_touch_point
  // returns false forever, which is the behaviour the fourth slot-confirm gate
  // in app_main reads through kiss_board_touch_ok().
  if (!kiss_board_touch_ok()) {
    ESP_LOGW(TAG, "touch: no controller, sampler not started");
    return;
  }
  // One priority rung ABOVE the caller, which is app_main -- the display loop
  // itself. A sampler the repaint can starve is this bug wearing a different
  // hat, so it has to be able to preempt a render, and it is cheap enough to
  // be allowed to: a poll with no finger on the glass is one short I2C read,
  // and one with a finger is about two milliseconds of it on the 4.3in's
  // 100 kHz touch bus. Nearly all of that is spent blocked on the driver's
  // semaphore with the CPU handed back, so what the render actually pays is
  // two context switches per sample.
  //
  // The priority is read off the caller rather than named, as main.c's prep
  // task does: CONFIG_ESP_MAIN_TASK_PRIORITY does not exist on IDF 6. CPU0,
  // beside the display loop, because CPU1 carries the camera's stream task at
  // priority 3 and would starve this one for as long as a preview runs. It is
  // not the highest thing on CPU0 -- the ISP pipeline's own task is created
  // without affinity at priority 11 -- which is the other reason the deadline
  // in sample_task has to survive being late.
  if (xTaskCreatePinnedToCore(sample_task, "kisstouch", 3072, NULL,
                              uxTaskPriorityGet(NULL) + 1, NULL, 0) != pdPASS) {
    // Degraded rather than dead: the readers fall back to reading the
    // controller themselves, which is what they did before this file existed.
    // A signer nobody can drive would be worse than the merged double tap.
    ESP_LOGE(TAG, "touch: sampler would not start, reading inline");
    return;
  }
  s_live = true;
  ESP_LOGI(TAG, "touch: sampled at %d Hz, lift after %d ms",
           1000 / SAMPLE_MS, SAMPLE_MS * UP_SAMPLES);
}

bool platform_read_touch(int *x, int *y)
{
  if (!s_live) return kiss_board_touch_point(x, y);
  portENTER_CRITICAL(&s_mux);
  bool p = kiss_touch_edge(x, y, (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
  portEXIT_CRITICAL(&s_mux);
  return p;
}

bool platform_read_touch_ui(int *x, int *y)
{
  if (!s_live) return kiss_board_touch_point(x, y);
  portENTER_CRITICAL(&s_mux);
  bool p = kiss_touch_level(x, y);
  portEXIT_CRITICAL(&s_mux);
  return p;
}
#endif  // ESP_PLATFORM

#if KISS_BLADE_LANDING
// Where the contact the gesture reader is being shown came down, for the game's
// blade (main.c, blade_land). The seat's contact and not the newest one: during
// a replay the two differ, and the landing has to belong to the contact whose
// points the reader is being handed. False when the seat holds none, and on
// the device when the sampler never started and the readers fell back to the
// controller, which keeps no landing to give. Here and not beside the sims'
// platform_read_touch, so the desktop builds read the same state the board does.
bool platform_touch_origin(int *x, int *y)
{
#ifdef ESP_PLATFORM
  if (!s_live) return false;
  portENTER_CRITICAL(&s_mux);
#endif
  bool held = s_seat.held;
  if (held) {
    *x = s_pt[s_seat.downs & PT_MASK].x0;
    *y = s_pt[s_seat.downs & PT_MASK].y0;
  }
#ifdef ESP_PLATFORM
  portEXIT_CRITICAL(&s_mux);
#endif
  return held;
}
#endif
