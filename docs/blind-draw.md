# Blind draw: making a wallet out of words you drew yourself

The device can create a wallet three ways. The camera and the dice both ask a
machine to produce randomness and ask you to trust it. **BLIND DRAW** does not.
You draw the words yourself, out of a bag or a shuffled stack, and the device
only does arithmetic it cannot fake.

This page is how to make the thing you draw from. The device can tell you to
print or cut the word list; it cannot hand you one.

## What you are actually making

Every Bitcoin recovery phrase is built from the same public list of 2048
English words. That list is called the **BIP39 wordlist**, it is the same list
in every wallet on earth, and there is nothing secret about it. You can print
it, publish it, tattoo it on a wall.

The secret is not the words. The secret is **which** of them came up, and **in
what order**. That is the only thing you are creating, and it is the only thing
you have to protect.

So the job is: make a physical copy of all 2048 words, mix it properly, and
pull words out without looking.

## Step 1: get the list

The list is in this repository:

```bash
less components/libwally-core/upstream/src/data/wordlists/english.txt
```

2048 lines, one word each, in alphabetical order. It is also published in BIP39
itself and mirrored by every wallet project. Any copy is as good as any other,
because it is identical everywhere by definition. If two copies of the list
differ, one of them is wrong.

## Step 2: make it physical

Two approaches, and neither is better than the other:

**Cut paper.** Print the list, cut it into 2048 slips, put them in a bag or a
bowl. Cheap, fast, and every slip is the same weight, which matters more than
it sounds like it should.

**3D print tiles.** Slower to make and far more durable. Tiles shake well in an
opaque container and survive being handled repeatedly, so this is the better
choice if you plan to do this more than once or hand it to someone else.

Whatever you make, two rules:

1. **It has to be all 2048.** This is the one that quietly ruins things. If you
   only cut 512 slips because that seemed like plenty, every draw is worth 9
   bits instead of 11, and eleven draws lose you 22 bits of security without
   changing anything you can see on the screen. The device cannot tell how many
   slips were in your bag.
2. **You must not be able to feel the difference.** Same paper, same cut, same
   tile. A slip that is slightly bigger is a slip your fingers will find.

## Step 3: mix, and draw without looking

Shuffle the stack, or shake the container, and take words out **without
looking at them until they are out**. That is the whole security model, and it
is why the mode is called a blind draw rather than a random one: "random" is a
feeling you cannot check, and "blind" is a fact about what you did.

Draw **11 words** for a 12 word phrase, or **23** for a 24 word phrase.

**Keep the order you drew them in.** Write them down 1, 2, 3, as they come out.
The order is part of the secret: sorting your slips alphabetically before typing
them throws away roughly 25 bits, and the device will notice and say so.

Putting each word back and remixing, or leaving it out, both work. The
difference between the two is four hundredths of a bit out of 121, so do
whichever is easier.

## Step 4: the device finds the last word

You will notice you only drew 11 words for a 12 word phrase. That is not a
mistake.

The final word of a recovery phrase is not free. Part of it is a **checksum** —
a few bits computed from all the words before it, so that a phrase with a typo
in it fails to load instead of quietly opening the wrong wallet. That is why
you cannot just draw the last word too: most of the words on the list would
produce a phrase that no wallet will accept.

So the device does the arithmetic. It tries all 2048 words against the 11 you
drew, and shows you every one that produces a valid phrase — **128 of them**
for a 12 word phrase, or **8** for a 24 word one. You pick. Any one of them
works, and none is better than another.

That is the only step where the device touches your phrase, and it never
chooses anything: it presents every legal option and waits for you.

## Step 5: check that the device is not lying

Because the last word is pure arithmetic, you can do it yourself and compare:

```bash
python3 tools/lastword.py <your eleven words>
```

If the device offers a word this does not, or hides one this shows, the
firmware is not doing what this page says.

**Use a throwaway draw for this, never a real wallet.** This step types your
words into a computer, and a computer is exactly what this whole mode exists to
avoid trusting.

## What the device will refuse, and why

Before your words become a wallet, the device looks at the draw itself. It is
not judging your taste, it is checking for the shapes a real blind draw
essentially never makes:

| What it sees | Why it is a problem |
| --- | --- |
| The same word repeatedly | There is no secret at all. Anyone can type it. |
| A short run typed over and over | Same problem, wearing a disguise. |
| Words sitting side by side on the list | The stack was never really mixed. |
| Words in alphabetical order | The order was half the secret. Sorting spent it. |
| More repeats than a full bag would give | Suggests the bag was not full. |

The first two are **refused outright** — those phrases carry nothing. The rest
are **warnings** you can read and then overrule, because each of them can happen
by chance to an honest draw, just very rarely.

What none of this can catch is a draw that merely looks random: a phrase you
memorised, a line from a song, words you picked while trying to feel
unpredictable. Human beings are not able to do this, and no check on the device
can tell the difference. Only a blind draw is random.
