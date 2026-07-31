# Tap entropy: the third source

Status: design approved 2026-07-30, not implemented.

New wallets currently hash two sources together: what the camera saw and what
the chip's TRNG produced. This spec adds a third that neither Espressif nor
this firmware can produce on its own — the timing of the user's own taps — and
makes it mandatory on every new wallet.

## Why a third source at all

The Coldcard Mk3 lost five years of wallets to a single manufacturer's RNG. A
refactor silently rebound seed generation from the STM32 hardware TRNG to a
software PRNG seeded from the chip's factory ID XORed with an uptime counter,
and every guard in the release passed because a PRNG's output is statistically
perfect. Only its seed was starved: 256 bits claimed, 32 achievable, ~16 if the
attacker had ever held the device and read the non secret unique ID.

Two lessons carry over here.

The first is structural. A hash of independent sources is as strong as its best
input, so the only fatal shape is depending on one. This device already mixes
two, which is why a lens cap cannot weaken a seed. But both of those sources
are manufactured by the same vendor and audited by the same firmware. Dice and
human hands are not, and taps are the cheapest way to reach for them: no extra
hardware, no user patience, no calibration against a lens.

The second is about measurement. Statistical randomness tests cannot detect a
low entropy seed, because they measure the output stream and the output stream
was never the problem. Anything this feature claims about its own quality has
to be verifiable at the source, not inferred from the bytes it emits. That
constraint drives most of the decisions below.

## What is actually unpredictable

Not the taps. Their timing.

Per counted tap the device folds one record:

    record = esp_cpu_get_cycle_count()   // 32-bit, 240MHz
           ‖ esp_timer_get_time()        // 64-bit, microseconds
           ‖ touch x, y                  // 2 x int16

The cycle counter carries the entropy. At 240MHz a human's tens of milliseconds
of inter tap variation spans tens of millions of cycles, so the low bits are
unpredictable even to someone standing over the user's shoulder with a stopwatch.
The microsecond timer is redundant on top of it and costs two words; it stays
because it is the value an auditor recognises. The touch coordinates are a bonus
and are not counted toward the budget: people tap the same spot, so the
coordinates are correlated across taps in a way the timing is not.

Each record is folded immediately:

    chain = SHA256(chain ‖ record)

Taps are never buffered. There is no array of timestamps in RAM for a later
attacker to find, and the fold is the same shape the camera path already uses
at `camera_spike.c:334`.

## Budget: 64 taps, 2 bits each, 128 bits claimed

Fixed and conservative. A tap whose arrival varies by even one millisecond
carries roughly eighteen bits at cycle resolution, so 2 bits is an order of
magnitude of headroom. The budget deliberately underclaims so that no user
rhythm and no future touch controller can turn the claim into a lie. It is a
floor chosen to survive being wrong, not an estimate.

64 taps is roughly 15 seconds. That buys a source that clears 128 bits on its
own, which is the point: if the camera and the TRNG both failed silently the way
Coldcard's did, the seed is still out of reach.

There is no adaptive meter and no Shannon estimate. The bar counts events that
happened, not a quality score. A count is a fact the code can prove; a quality
score is a claim that would need calibrating against human behaviour and would
carry exactly the false assurance the postmortem describes. This is the same
reason the camera screen's Shannon meter is documented as a quality prompt
rather than a security control.

**Debounce.** A tap arriving less than 30ms after the previous one does not
count. That window is below deliberate human repetition and above the touch
controller's drag and bounce artifacts, so it removes records whose timing is a
property of the hardware rather than the hand.

**Nothing is ever refused.** No regularity test, no rhythm detection, no
statistical gate. A screen that can permanently refuse is a device that cannot
make a wallet, and a metronome-perfect user still contributes the cycle level
jitter the budget is priced on. A stuck or dead touch panel simply never fills
the bar, which explains itself without an error path.

## Flow

Today `CAPTURE` on the randomness screen generates the seed. It now advances.

    choose length -> randomness (camera + chip) -> tap (source 3) -> words

The mash stage is mandatory. There is no skip, and no menu that would let a user
land on a single source, because a menu of sources reintroduces the one shape
this feature exists to prevent.

## Randomness screen: minimal changes

The right column is full — cards at y=128 and y=240, the equation at y=344, the
action row at 404 — and a third 96px card does not fit. It does not need to.

- `STR_W_RAND_S` becomes three sources rather than two.
- The equation becomes `1 + 2 + 3 -> 12 WORDS`, with chip `3` drawn dim via the
  existing `wt_chip(row, "3", false)` styling. A dim chip in an equation reads
  as a promise the next screen keeps.
- `STR_W_ENT_MIX_B`, the WHY TWO SOURCES explainer, becomes WHY THREE and gains
  the line that matters: an attacker now has to beat something the manufacturer
  never touched.
- Cards 1 and 2 do not move.
- `CAPTURE` stays the primary pill and keeps its readiness gate. Only its
  callback changes, from generate to advance.

## Tap screen

Landscape 800x480, built from the existing kit (`wt_screen`, `wt_lbl`,
`mk_pill`, `WT_ACTION_Y`).

**One card is the target.** Roughly 600x260, centred, `WT_PANEL` fill with a
`WT_EDGE` border. The card itself is clickable, so there is nothing to aim at
and no missed touch. It is drawn as a raised surface rather than a pill because
a 600px pill would read as a button that moves money.

**Inside the card, top to bottom:**

- Source caption in the established form: `SOURCE 3   YOUR OWN HANDS`.
- A 64 segment bar. One segment lights per counted tap, in `WT_OK`, unlit
  segments in `WT_EDGE`. Segments rather than a continuous fill because a
  segment is a countable event and a smooth bar implies a measurement.
- The count in mono: `31 / 64`, using `wt_font_mono28()`.
- One note line, `wt_font14()` in `WT_MUT`: tap anywhere on the card, any
  rhythm.

**Feedback per tap.** The struck segment lights and the card border flashes to
the accent for one frame. Nothing else moves. The bar is the whole feedback
loop; a counter that also animates would compete with it.

**On the 64th tap** the screen mixes, holds the full bar for ~400ms so the
completion is seen rather than inferred, and advances to the words screen.

**Action row: CANCEL only**, at the standard `WT_BACK_X`. Same rule the words
screen documents: no screen without an exit. Cancelling here stages nothing and
destroys nothing, since the seed does not exist until the mix runs.

**Copy** follows the house rules: lower case body, no hyphens, no string that
restates the title or a value beside it. Draft English:

    STR_W_ENT_TAP_T     "TAP TO ADD RANDOMNESS"
    STR_W_ENT_TAP_S     "your timing is the part no manufacturer can guess."
    STR_W_ENT_SRC3_CAP  "SOURCE 3   YOUR OWN HANDS"
    STR_W_ENT_TAP_NOTE  "tap anywhere on the card. any rhythm."
    STR_W_ENT_TAP_CNT   "%d / %d"

New keys land in `i18n_keys.h` and need entries across every table in
`i18n_tables.c`. The count format string carries no words so it needs no
translation, but it stays a key so a language that reorders numerals can.

## Mixing

`wallet_entropy_mix()` takes exactly two 32 byte inputs. Add a sibling in
`wallet_crypto.c`:

    int wallet_entropy_mix3(const uint8_t a[32], const uint8_t b[32],
                            const uint8_t c[32], uint8_t out[32]);
    // out = SHA256(a ‖ b ‖ c)

A flat three input hash, not nested two argument calls. Nesting is
cryptographically equivalent and produces a call site a reader has to unpick to
answer the only question that matters, which is what went into the seed. The
two argument function stays for the camera's per frame fold, which genuinely
takes two inputs.

Ordering is fixed as camera, chip, taps, matching the source numbers on screen.

The mix runs on the tap screen, on the 64th tap. The camera hands its chain
across instead of finishing the seed itself, so `camera_spike.c:351` stops
calling `wallet_entropy_mix` and exposes the accumulated chain to the caller.
Both intermediate chains are zeroed with `wally_bzero` once the mix returns.

## Camera failure

Unchanged and now less severe. The camera failure branch already keeps cards 1
and 2 truthful and reports the error in the preview column. The tap screen runs
regardless, so a device with a dead camera now builds a seed from two
independent sources instead of one.

## Testing

**Unit, on host.** `wallet_entropy_mix3` against a known answer vector computed
independently. Same fixed inputs must produce the same output; changing any one
input byte must change the output. Ordering must matter: `mix3(a,b,c)` and
`mix3(c,b,a)` must differ.

**Unit, on host.** The fold and debounce logic, extracted so it takes injected
timestamps rather than reading the clock: 64 records in produce a chain
distinct from 63; a record 29ms after its predecessor does not advance the
count; one at 31ms does.

Explicitly not tested: the statistical distribution of the output. Such a test
would pass against a starved source and is the exact false assurance this spec
exists to avoid. What gets verified is structural — that the taps reach the
hash, that the count is honest, and that the mix consumes all three inputs.

**Device.** Required, and the reason is in the next section.

## Device test verdict

**DEVICE TEST: REQUIRED.**

Host unit tests cannot substitute here and passing CI is not the verdict. The
feature reads a touch controller and a CPU cycle counter, and its security claim
rests on the resolution of both. Flows to exercise on hardware:

1. Sixty four real taps fill the bar exactly once each, with no double counts
   from panel bounce and no dropped taps during rapid mashing.
2. Logged cycle counter deltas across a real session vary in their low bits.
   If the touch driver reports on a coarse tick, the timing entropy is not
   there and the budget is wrong. This is the assumption most likely to fail
   and it cannot be checked off device.
3. A tap held and dragged across the card counts once, not once per move event.
4. CANCEL from a part filled bar returns to the randomness screen and stages
   nothing.
5. The full path with the camera physically covered, confirming the tap screen
   still completes.
6. Fingerprint differs across two consecutive wallets created with deliberately
   identical scenes and identical tap counts.

## Open question

The exact resolution of touch events as delivered to LVGL on this panel is not
yet measured. The 2 bits per tap budget assumes the timestamp is taken in the
event callback at cycle resolution rather than at a coarse polling tick. Device
test item 2 settles it. If the resolution turns out to be coarse, the fix is to
timestamp in the touch ISR rather than to raise the tap count.
