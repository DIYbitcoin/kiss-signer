# Cold storage for ecash

Cashu ecash is digital cash issued by a mint. Normally it sits in an app on
your phone like cash in a pocket: anyone who copies the app's data can spend
it. This page shows how to keep ecash that only your KISS Signer can spend,
with a web page holding it and the signer approving every spend by QR code.

Test mint and fake sats only for now. Nothing here has been used with real
bitcoin.

## What it protects, and what it does not

**Protects against:** someone who gets into the phone or computer holding
your ecash, or a copy of its storage. The ecash is locked to keys made from
your signer's seed words (Cashu NUT-11 and NUT-13), so without a signature
from the signer it cannot be spent.

**Does not protect against:**

- **The mint.** The mint holds the real bitcoin behind every bit of ecash. If
  it shuts down or cheats, the ecash is worth nothing, locked or not. Keep
  only small amounts at any one mint.
- **Losing the page's storage.** The vault page keeps the ecash in your
  browser's storage. Clearing that storage, or losing the device, loses the
  ecash: your seed words alone cannot bring it back yet.
- **A dishonest page throwing ecash away.** It cannot spend your ecash, but it
  can delete it, or make your change worthless.
- **Who you pay.** The signer shows how many sats leave and how many come back
  to it. It cannot know who receives the rest, the same as when it signs a
  bitcoin transaction.

## What you need

- A KISS Signer with seed words on it, unlocked to its home screen.
- The vault page, [docs/ecash](ecash/index.html), in Brave or Chrome on a
  computer or phone with a camera.

## 1. Pair

1. Open the vault page.
2. On the signer, tap **SIGN** and scan the pairing QR on the page.
3. The signer shows its ecash key as a QR: on the page tap **Scan** and show
   it to the camera.

The page now knows the signer's public ecash key (an xpub). It can lock ecash
to the signer, but it cannot spend it.

## 2. Receive

Three ways in, all ending up locked to the signer:

- **Get test sats:** the test mint hands out fake sats, locked straight away.
- **Paste a token** someone sent you. An ordinary token is swapped at the
  mint into ecash locked to the signer, for a fee of a sat or so.
- **The tip jar** (step 4). Tips arrive already locked.

## 3. Spend

1. On the page, type an amount and tap **Make spend request**.
2. On the signer, tap **SIGN** and scan the moving QR.
3. Check the screen: the sats **sending**, the sats that **come back** as
   change, and the mint, as the page names it.
4. Slide to sign. The signer shows an answer QR.
5. On the page tap **Scan answer** and show it the signer.
6. The page swaps at the mint and shows a token for the person you are paying.

To stop instead, press **BACK** on the signer and **Cancel** on the page.
Nothing was signed.

## 4. Tip jar

The page can show a payment request (Cashu NUT-18) to print or post. Anyone
pays it from a Cashu app such as cashu.me, and the request asks their app to
lock the tip to your signer. The app delivers the tip to the page over Nostr
(NIP-17), and the page picks it up while it is open, including tips sent
while it was closed. A tip someone sends as a token can be pasted into
**Receive** instead. Either way:

- A tip that arrived **locked** is kept as it is, with no fee. The page first
  checks the mint's proof that it issued the tip and that none of it is spent.
  The sender cannot take it back.
- A tip that arrived **unlocked**, because the sender's app ignored the
  request, is swapped into locked ecash at once.
- A tip locked to anything else is refused.

All tips share one key, so one spend can take them all. The cost is that the
mint can tell the tips went to the same place.

## How it works

The QR codes follow the [ecash spend request](specs/ecash-spend-request.md)
format, which also lists every field a dishonest page could forge and what
the signer does about each one.
