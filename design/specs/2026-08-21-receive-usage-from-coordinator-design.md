# Address usage from the coordinator

Status: design approved 2026-08-21, revised the same day after a UX pass.
Spans two repositories: this one and the BDK coordinator
(`kiss-bdk-coordinator`).

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
USED on its own evidence, says UNUSED only once a coordinator has told it what
the chain shows, and when it has not been told it says so.

## The errand is one the owner already runs

The first pass at this put usage on its own QR behind its own command. That was
wrong twice over. It cost a second camera trip for one errand — the owner
already scans their coordinator's address at VERIFY — and worse, nothing on the
Receive screen would ever have said the badge could know more than it does, so a
first time owner had no way to discover the feature existed at all.

So the usage rides along with the address, in the QR the owner already scans, at
the button they already use. One payload, one scan, one gesture that was already
taught by the pairing screen's own copy: "then prove it: RECEIVE > VERIFY - scan
the coordinator wallet's first address to confirm it's yours." The badge
updating is a side effect of the check the owner came to do.

`kiss_scan_open_raw` already hands the first decoded payload to a callback as
raw text, so no new camera door is built. Routing happens in `vfy_result`
**before** `vfy_norm` runs, on the raw scanned text: anything without the magic
falls through to the address path exactly as it does today.

## Wire format

```
KISSU1 EC5A4595 1 0 29 1234567 TB1Q...
```

| field | meaning |
| --- | --- |
| `KISSU1` | magic and version. A new version is a new magic, never a flag |
| `EC5A4595` | master fingerprint, 8 uppercase hex |
| `1` | network: 0 mainnet, 1 testnet |
| `0` | script: `WSCRIPT_NATIVE` 0, `WSCRIPT_NESTED` 1, `WSCRIPT_LEGACY` 2 |
| `29` | highest used external index, or `-1` for a chain that shows none used |
| `1234567` | chain tip height the coordinator synced to |
| `TB1Q...` | the address to verify: next unused, uppercase for bech32 |

Six fixed fields then the address, which is the only variable length one and so
goes last. Bech32 is emitted uppercase, which BIP173 already recommends inside a
QR and which `vfy_norm` already folds back down, so a native segwit payload
stays inside the QR alphanumeric charset. A base58 address cannot be uppercased
and drops the QR to byte mode, which costs a little density and nothing else.

There is no checksum: a QR that decodes has already passed Reed Solomon, and the
strict field parse rejects what survives that. This is display data riding with
an address that gets re-derived anyway, not a signing input.

The external chain only. Nothing on this device badges a change address —
`mark_used_receives` skips change inputs by construction, and the Receive screen
shows external addresses. An internal mark would arrive with no reader.

`-1` is a real answer, not an absence. A freshly synced empty wallet genuinely
shows nothing used, and every address on it is truthfully UNUSED. That is
different from having never been told, which the payload cannot express because
it is the state of not having a payload.

**The index field says nothing about the scanned address.** It is a fact about
the wallet, not about this address, and `vfy_find` must go on re-deriving and
searching for the address exactly as it does now. Nothing in the payload is
allowed to shortcut the ownership answer; that answer is the reason the screen
exists.

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
correction after a reorg or a rebuilt wallet still lands.

That ordering job is the whole of what the height is for. It never reaches the
glass: this signer has no chain view and cannot turn 1234567 into "a week ago",
and a number the device cannot judge and the owner cannot place is not a
freshness signal, it is a prop.

The local high water mark keeps its own monotonic rule, unchanged. It records
what this device witnessed and nothing a camera says can lower it.

## What the screen says

```c
int high = local;                      // what this device saw itself
if (known && chain > high) high = chain;

if ((int)s_idx <= high)  -> ALREADY USED    (WT_WARN)
else if (known)          -> UNUSED          (WT_OK)
else                     -> USAGE UNKNOWN   (WT_MUT)
```

The order carries the argument. USED still appears with no coordinator data,
because an address this signer signed a spend from is on chain whatever any
desktop says. Only UNUSED waits. A wrong or stale coordinator value can move
addresses into USED, which sends the owner to a fresher address than they
needed, and can never move a locally witnessed one out of it.

The third state is a chip in `WT_MUT` and not an absence. Blank was the honest
answer to "is this address used" but it was silent about its own silence: the
screen would have looked finished, and nothing on it would ever have told an
owner that the answer was obtainable. USAGE UNKNOWN is not a claim about the
address, it is the device saying what it does not know, which is the one thing
it can say truthfully here. It reuses `wt_state_chip` at the position the chip
already occupies, so no layout moves.

Under the QR card in the left column, one muted line carries the provenance and,
when there is none, the way to get it. The card sits at (48, 112) 238x202 and
ends at 314; content runs to `WT_CONTENT_BOTTOM` 398, so the band is free and
two font14 lines fit. font14 is correct here and is the narrow case the house
rules allow: metadata about the screen's knowledge, not a sentence the owner
must read to act.

Known reads as what is actually known — "coordinator: used up to #29" — and not
as a count. A high water mark of 29 does not mean thirty addresses were used,
because the mark cannot see gaps, and a line claiming a count would be inventing
one. Unknown reads as the instruction, naming the button beside it, and wraps to
the second line. Final copy goes through `kissfit`; a fit helper landing on
font14 here is expected, since font14 is the intended size, but a helper that
has to wrap past two lines means the copy is too long and words get cut.

The VERIFY result screen reuses the shape `vfy_result` already builds and now
answers two things in one card. The ownership answer stays exactly where it is
and keeps its headline. What the usage half adds is one muted line below it:
what was recorded, or that the payload was older than what is stored and so
nothing moved, which is a state and not an error.

A fingerprint that does not match does **not** refuse the scan. The address
still gets its ownership answer, because that is what the owner came for and it
is answerable without trusting anything in the payload; only the usage half is
dropped, and the line says the coordinator is watching different keys. A payload
naming a network or script the screen is not currently showing is still stored
in the bucket it names, and the line says which one, so "updated" never appears
over a badge that did not move.

## What the coordinator does

`parse_kiss_descriptor` returns the origin fields that `split_kiss_descriptor`
already validates at `src/lib.rs:66` and then discards — fingerprint and
purpose, alongside the two descriptors. `split_kiss_descriptor` stays as a thin
wrapper so its tests do not move.

No new subcommand. `address` grows `--qr`, which is what `--qr` already means in
this CLI: `create --qr` shows a PSBT for KISS's camera, and this shows the
address and its usage for the same camera. Plain `address` still prints text and
nothing else, so the artifact an owner might hand to a payer is unchanged and no
usage rides out with it.

```rust
let height = wallet.latest_checkpoint().height();
let high = wallet.spk_index().last_used_index(KeychainKind::External);
```

Both are present in `bdk_wallet` 3.1.0 and `bdk_chain` 0.23.3. `--qr` refuses
when `height == 0`: an unsynced wallet would otherwise emit a confident payload
about a chain it has never seen, and that payload would win the height gate
against nothing and then block the real one. `DEMO.md` already runs `sync`
before `address`, so the order this enforces is the order documented.

## Trust model

The payload is display data. It must never be trusted for anything else, and by
construction it is not: the index chooses a chip's text and colour, and no
derivation, no address search and no signing path reads it. The address that
rides with it is re-derived on this device exactly as before.

The failure modes, worst first:

**An inflated index.** Someone with camera access who knows the fingerprint
shows a payload claiming a high index. Addresses badge USED and the owner moves
to a fresh one. That is griefing, not a funds risk, and it costs physical access
to a device that is already unlocked. The accepted index is capped at 100000 —
not a real wallet's ceiling but an absurdity bound, so a scanned number cannot
strand the chip somewhere no owner could walk back from with NEXT. A payload
above the cap is refused like any other malformed field.

**Stale data.** UNUSED on an address funded since the last sync. This is the
tolerable failure named at the outset. It is not stamped on the glass, because
the stamp available was a block height the device cannot judge; what the owner
gets instead is a screen that never claims more than a coordinator told it.

**A gap below the mark.** An address revealed but never funded reads USED. The
high water mark cannot see gaps. It is cosmetic and it errs toward a fresh
address, which is the direction to err.

**A wrong fingerprint.** The usage half is dropped and said so; the ownership
answer is unaffected.

## Verification

Sim tests beside the existing `kiss_usage` cases in `sim/test_crypto.c`: the
parser accepts a well formed payload and rejects wrong magic, wrong field count,
truncation, an index above the cap and a missing address; a fingerprint mismatch
drops usage while the ownership answer still lands; the height gate ignores an
older payload and accepts a newer one that lowers the index; the badge shows
USAGE UNKNOWN with no data, UNUSED above the mark with data, and USED at or
below it on local evidence alone; `-1` badges every address UNUSED; a payload
whose index claims one thing and whose address derives at another still reports
the derived index, pinning that the index field cannot shortcut `vfy_find`; and
a wipe clears the chain fields with the rest. Parser fuzzing joins
`sim/test_fuzz.c`.

Gates: `kisstest`, `kissfuzz`, `kissfit` for the new strings, `overlapcheck`,
the sim tap walk, and the Docker ESP-IDF build. On the coordinator, `cargo test`
over payload formatting, uppercase bech32 emission, origin extraction and the
unsynced refusal.

New strings are English only in `i18n/en.json`, `main/i18n_tables.c` and
`main/i18n_keys.h`, leaving the other twenty locales to a sweep, which is how
the SeedQR removal left them.

## Device test verdict

**DEVICE TEST: REQUIRED.** This puts a new payload through the camera decode
path and new copy onto a dense screen, and no gate sees either. The flows: scan
an `address --qr` payload off a real monitor at RECEIVE > VERIFY and confirm it
both answers ownership and records usage; read the badge on an address above and
below the mark, and on a device that has never been told; confirm the line under
the QR fits in 238 without passing two lines and clears `WT_CONTENT_BOTTOM`;
scan a payload whose fingerprint does not match and confirm the ownership answer
still lands; scan the same QR twice and confirm the second reports that nothing
moved.

Passing gates are not this verdict and do not substitute for it.
