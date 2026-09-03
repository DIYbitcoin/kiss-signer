# The card in the box

The one surface this project ships that is not glass, and the only place the
way into the signer is ever taught from the outside.

It is specified here, in the repository, for the reason every other surface is:
the copy was living wherever it was living, in nobody's review and under no
gate, while being the single thing standing between a new owner and a device
that looks like a fruit game.

## Why it exists at all

The signer hides behind a game. That is the point — a box that announces itself
as a bitcoin signing device is a box worth taking — and it means the way in
cannot be printed on the glass, because printing it there is the same as not
hiding. A secret that has to stay secret is still allowed a clear first
experience. It just gets delivered on paper.

## The copy

Four things, in this order, and nothing else.

> ### It shows a game
>
> This device opens on a fruit game. That is on purpose.
>
> ### Draw KISS on it
>
> Draw the letters K I S S across the game, with your finger.
>
> ### That drawing is the only way in
>
> Nothing else opens the signer. There is no button and no menu.
>
> ### Keep this card
>
> The game will never offer to tell you. Once you are in,
> SETTINGS > WAYS IN says it again.

## The rules it is written under

The same ones the glass is written under, because a reader who meets the card
first and the screens second must not have to unlearn anything.

- **Ordinary words, said the way somebody would say them out loud.** The reader
  is holding a box they have just unwrapped.
- **A sentence is under fourteen words.**
- **No metaphor.** The device does not "wake", "sleep" or "hide" anything.
- **The names in full.** It is a **signer**, never a wallet. The gesture is a
  **drawing**, which is what the device calls it on every screen that mentions
  it — `YOUR DRAWING IS SET`, `only your own drawing`.
- **No hyphens.**

## What it deliberately does not say

- **Not the passphrase, the decoy, or the seed words.** All three are taught on
  the glass, at the moment they matter, by screens written for them. A card
  that explains the whole device is a card nobody reads to the end, and the one
  line that matters is buried in it.
- **Not "write your seed words down".** The setup flow says that, on the screen
  where the words are on the display, which is the only moment it can be acted
  on.
- **Not a warning about losing the drawing.** The device covers that itself:
  `IF YOU FORGET / no reset, seed words only`, on the screen that sets it.

## The fourth line is a promise the device keeps

"The game will never offer to tell you" has to stay true. Nothing on the cover
may hint at the signer — that was tried during this work and reverted, because
`kiss_seed_exists()` is false on an AMNESIC signer with no session loaded and
on an SD signer with its card out, so a cover that offers a way in whenever
there are no keys wears a signer's name on exactly the two modes that need the
cover most.

The second half is the part that keeps the card from being a single point of
failure: once an owner is inside, `SETTINGS > WAYS IN` shows what opens the
decoy and what opens the real keys, and the drawing can be changed there.

## Printing

Not specified here, and deliberately. This file owns the WORDS and their order;
whoever lays the card out owns the paper, and neither should be able to change
the other by accident.
