# Address usage from the coordinator

Status: design approved 2026-08-21. Spans two repositories: this one and the
BDK coordinator (`kiss-bdk-coordinator`).

## Why

The Receive screen badges every address UNUSED. A signer has no chain view, so
it cannot know that. Observed on a wallet with real history at indices 0..29:
all thirty read UNUSED on glass while the coordinator's `next_unused_address()`
correctly answered 30.

The chip is not new and the reasoning behind it is already written down at
`main/kiss_recv.c:249`. It argues that USED and UNUSED are the words every
wallet uses, and that UNUSED means "no record here", which is what a watch only
wallet means by it too. The first half holds. The second does not: a watch only
wallet says UNUSED because it looked at the chain and found nothing, and this
device says it because it has never looked at anything. Two different claims
wearing one word, on the screen whose subtitle asks the owner to trust what they
see here.

Splitting the claim is the whole change. USED is a positive fact this signer can
prove, because `mark_used_receives` records the index of every receive input it
signs and spending an output proves it was funded. UNUSED is a negative fact,
and negative facts about the chain need the chain. So the signer keeps saying
USED on its own evidence, and says UNUSED only once a coordinator has told it
what the chain shows. Absent that, it says nothing, which is the honest answer
and the one the current code does not have.

## What the coordinator sends

Pairing runs one way: the signer shows its descriptor and the desktop scans it.
There is no reverse channel at pair time and no reason to build one — the data
only exists after `sync`, long after pairing, and `kiss_scan_open_raw` already
hands the first decoded QR payload to a callback as raw text. RECEIVE > VERIFY
uses it today, and the pairing screen's own copy already teaches the gesture:
"then prove it: RECEIVE > VERIFY - scan the coordinator wallet's first address
to confirm it's yours."

So the update arrives through VERIFY. Routing happens in `vfy_result` **before**
`vfy_norm` runs, on the raw scanned text: anything that is not this magic falls
through to the address path exactly as it does today, and the ownership check is
untouched.

The wire format is one line of text in a single static QR:

```
KISSU1 EC5A4595 1 0 29 1234567
```

| field | meaning |
| --- | --- |
| `KISSU1` | magic and version. A new version is a new magic, never a flag |
| `EC5A4595` | master fingerprint, 8 uppercase hex |
| `1` | network: 0 mainnet, 1 testnet |
| `0` | script: `WSCRIPT_NATIVE` 0, `WSCRIPT_NESTED` 1, `WSCRIPT_LEGACY` 2 |
| `29` | highest used external index, or `-1` for a chain that shows none used |
| `1234567` | chain tip height the coordinator synced to |

Uppercase hex and spaces keep the payload inside the QR alphanumeric charset.
There is no checksum: a QR that decodes has already passed Reed Solomon, and the
strict field parse rejects what survives that. This is display data, not a
signing input, and a checksum here would buy nothing the transport does not
already provide.

The external chain only. Nothing on this device badges a change address —
`mark_used_receives` skips change inputs by construction, and the Receive screen
shows external addresses. An internal mark would arrive with no reader.

`-1` is a real answer, not an absence. A freshly synced empty wallet genuinely
shows nothing used, and every address on it is truthfully UNUSED. That is
different from having never been told, which the payload cannot express because
it is the state of not having a payload.

## What the signer stores

`kiss_usage` gains a second fact per bucket, in the same `kissu` namespace so
the existing wipe, persist switch and amnesic handling cover it without new
paths:

```c
// What a coordinator claims the chain shows, and the height it claimed it at.
// Separate from kiss_usage_high: that is what THIS device saw, and the two
// answer different questions on the Receive screen.
int kiss_usage_chain_known(const uint8_t fp[4], int testnet, int script,
                           int *high, uint32_t *height);
int kiss_usage_chain_set(const uint8_t fp[4], int testnet, int script,
                         int high, uint32_t height);   // 1 = accepted
```

NVS keys `c<key>` and `h<key>`, 11 chars against the 15 limit. The index is
stored as `high + 1` so that 0 means "chain shows none used" and no signed value
reaches NVS. The session table's `struct usage_row` grows two fields rather than
gaining two rows, so `UMAX` still buys 32 buckets and amnesic mode costs the
same RAM it does now.

**The accept rule is height gated.** `kiss_usage_chain_set` accepts only when
the payload's height is strictly greater than the stored height, and then takes
the index even if it is lower. Re showing an old QR changes nothing. A genuine
correction after a reorg or a rebuilt wallet still lands. This is the one place
the height earns its bytes; see the trust model on why it earns little else.

The local high water mark keeps its own monotonic rule, unchanged. It records
what this device witnessed and nothing a camera says can lower it.

## What the screen says

```c
int high = local;                      // what this device saw itself
if (known && chain > high) high = chain;

if ((int)s_idx <= high)  -> ALREADY USED   (WT_WARN)
else if (known)          -> UNUSED         (WT_OK)
else                     -> no chip at all
```

The order carries the argument. USED still appears with no coordinator data,
because an address this signer signed a spend from is on chain whatever any
desktop says. Only UNUSED waits. A wrong or stale coordinator value can move
addresses into USED, which sends the owner to a fresh address, and can never
move a locally witnessed one out of it.

The freshness line goes under the QR card in the left column. The card sits at
(48, 112) 238x202 and ends at 314; content runs to `WT_CONTENT_BOTTOM` 398, so
the band is free. font14 muted, which is correct here and is the narrow case the
house rules allow: this is metadata about the screen's knowledge, not a sentence
the owner has to read to act. It renders only when `known`.

The VERIFY result screen reuses the shape `vfy_result` already builds. Accepted
says what was recorded and at which height. A payload older than what is stored
says so and is not an error. A fingerprint that does not match is refused
visibly, naming the mismatch rather than failing silent. A payload for a network
or script the screen is not currently showing is still stored in the bucket it
names, and the result says which one, so that "updated" never appears over a
badge that did not move.

## What the coordinator does

`parse_kiss_descriptor` returns the origin fields that `split_kiss_descriptor`
already validates at `src/lib.rs:66` and then discards — fingerprint and
purpose, alongside the two descriptors. `split_kiss_descriptor` stays as a thin
wrapper so its tests do not move.

A new `usage` subcommand, no network call, the same shape as `address` and
`balance`:

```rust
let height = wallet.latest_checkpoint().height();
let high = wallet.spk_index().last_used_index(KeychainKind::External);
```

Both are present in `bdk_wallet` 3.1.0 and `bdk_chain` 0.23.3. It refuses when
`height == 0`, because an unsynced wallet would otherwise emit a confident
payload about a chain it has never seen, and that payload would then win the
height gate against nothing and sit there.

## Trust model

The payload is display data. It must never be trusted for anything else, and by
construction it is not: the index chooses a chip's text and colour, and no
derivation, no address, and no signing path reads it.

The failure modes, worst first:

**An inflated index.** Someone with camera access who knows the fingerprint
shows a payload claiming a high index. Addresses badge USED and the owner moves
to a fresh one. That is griefing, not a funds risk, and it costs physical access
to a device that is already unlocked. The accepted index is capped at 100000 —
not a real wallet's ceiling but an absurdity bound, so a scanned number cannot
strand the chip somewhere no owner could walk back from with NEXT. A payload
above the cap is refused like any other malformed field.

**Stale data.** UNUSED on an address funded since the last sync. This is the
tolerable failure named at the outset, and it is now stamped with the block it
came from rather than being silent.

**A gap below the mark.** An address revealed but never funded reads USED. The
high water mark cannot see gaps. It is cosmetic and it errs toward a fresh
address, which is the direction to err.

**A wrong fingerprint.** Refused on screen.

The height's device facing job is ordering and nothing more. This signer has no
chain view, so it cannot judge whether a height is old — it cannot turn 1234567
into "a week ago". On glass the number is provenance the owner can compare
against their coordinator, which is worth the line but is not a staleness alarm,
and the copy must not pretend otherwise.

## Verification

Sim tests beside the existing `kiss_usage` cases in `sim/test_crypto.c`: the
parser accepts a well formed payload and rejects wrong magic, wrong field count,
truncation and out of range values; a fingerprint mismatch is refused; the
height gate ignores an older payload and accepts a newer one that lowers the
index; the badge is silent with no data, UNUSED above the mark with data, USED
at or below it on local evidence alone; `-1` badges every address UNUSED; and a
wipe clears the chain fields with the rest. Parser fuzzing joins
`sim/test_fuzz.c`.

Gates: `kisstest`, `kissfuzz`, `kissfit` for the new string, `overlapcheck`, the
sim tap walk, and the Docker ESP-IDF build. On the coordinator, `cargo test`
over payload formatting, origin extraction and the unsynced refusal.

New strings are English only in `i18n/en.json`, `main/i18n_tables.c` and
`main/i18n_keys.h`, leaving the other twenty locales to a sweep, which is how
the SeedQR removal left them.

## Device test verdict

**DEVICE TEST: REQUIRED.** This puts a new payload through the camera decode
path and a new label onto a dense screen, and no gate sees either. The flows:
scan a `usage` QR off a real monitor at RECEIVE > VERIFY and confirm it decodes
and records; read the badge on an address above and below the mark; confirm the
freshness line fits under the QR and clears `WT_CONTENT_BOTTOM`; scan a
fingerprint that does not match and read the refusal; scan the same QR twice and
confirm the second is reported as already up to date.

Passing gates are not this verdict and do not substitute for it.
