# Ecash spend request

Status: review and sign in kiss_ecash.c, the screens in kiss_ecash_ui.c,
the coordinator in docs/ecash; not yet tested on a device, 2026-11-13.

A way for an offline signer to hold Cashu ecash it never sees. A phone or
browser page (the coordinator) holds ecash locked to this signer's keys
(NUT-11 pay to public key, keys derived per NUT-13 and exported as one xpub).
It cannot spend that ecash. To spend, it shows the signer a spend request as a
QR; the signer checks it, shows what leaves and what comes back, signs, and
shows the signature back as a QR. The coordinator puts the signature in the
swap and sends it to the mint.

This is the ecash version of a PSBT, and it has a PSBT's limit: the signer can
prove which outputs come back to it, and can only count the rest.

## The flow, end to end

1. **Pair.** The signer exports `[fp/129373h/10h/0h/0h]xpub...`. The
   coordinator derives the key at counter `i` for each incoming payment.
2. **Receive.** The coordinator asks senders (a NUT-18 payment request with a
   `nut10` lock, or its own swap) for ecash locked to key `i` with exactly the
   tags `[["sigflag","SIG_ALL"]]`, and refuses anything else it is given.
3. **Spend.** The coordinator builds a swap: inputs are proofs all locked to
   one key `i` (NUT-11 requires every SIG_ALL input to share `data` and
   `tags`); outputs are the payment's blinded messages plus change blinded
   messages locked back to this signer.
4. **Request.** It encodes the request below and shows it as a QR.
5. **Review.** The signer parses it, refuses anything outside this spec,
   recomputes every change output and shows: the mint and unit as the phone
   states them, the sats leaving, the sats coming back as change.
6. **Sign.** On a hold to sign, the signer computes the NUT-11 SIG_ALL message
   from the reviewed request and signs it with the key at counter `i`.
7. **Reply.** The signer shows the reply QR. The coordinator checks the hash
   matches its swap, sets `witness` on the first input, and submits the swap.

Rules already in this flow that the request must meet: NUT-11 (SIG_ALL inputs
share data and tags; the message is the concatenation of the input secrets and
C, then each output's amount and B_; signatures are BIP340 over its SHA256),
NUT-00 (B_ = hash_to_curve(secret) + r*G), NUT-13 (the key path), and the mint's
own balance rule (inputs = outputs + fees), which is what makes the output
amounts the real value moved.

## The request

UTF-8 JSON, at most 4096 bytes (the QR transport's limit). A strict subset:
no floats, no negative numbers, no duplicate keys, no keys outside this list,
and the only string escapes are `\"` and `\\`. Anything else is refused, not
repaired, so the signer and the mint can never read one string two ways.

```json
{
  "v": 1,
  "mint": "https://testnut.cashu.space",
  "unit": "sat",
  "counter": 7,
  "inputs": [
    {"secret": "[\"P2PK\",{\"nonce\":\"...\",\"data\":\"02...\",\"tags\":[[\"sigflag\",\"SIG_ALL\"]]}]",
     "C": "02..."}
  ],
  "outputs": [
    {"amount": 16, "B_": "03..."},
    {"amount": 4, "B_": "02...",
     "change": {"counter": 8, "nonce": "<64 hex>", "r": "<64 hex>"}}
  ]
}
```

- `v`: 1.
- `mint`: shown, never trusted. It is not in the signed message.
- `unit`: exactly `"sat"`. It is not in the signed message either, so it
  cannot be checked, only fixed: any other value is refused.
- `counter`: the NUT-13 counter of the key every input is locked to.
- `inputs`: 1 to 64, in swap order. `secret` verbatim; `C` lowercase hex.
- `outputs`: 1 to 64, in swap order. `amount` 1 to 2^53, `B_` lowercase hex.
  An output with `change` claims to come back to this signer; one without it
  is counted as leaving.

A change output's secret is not sent. The signer builds it itself, as
`["P2PK",{"nonce":"<nonce>","data":"<key at change.counter>","tags":[["sigflag","SIG_ALL"]]}]`
with no spaces, and the coordinator must use exactly that string.

## The reply

```json
{"v":1,"hash":"<64 hex SIG_ALL message hash>","sig":"<128 hex>"}
```

The coordinator puts `{"signatures":["<sig>"]}` in the first input's `witness`.

## What a forged request can do

The coordinator is assumed to be the thief. Every field it supplies, and what
forging, omitting or pre filling it does:

| Field | Forged, omitted or pre filled | What holds |
| --- | --- | --- |
| `counter` | a key that is not the inputs' | every input's `data` must equal the key derived here; refused otherwise |
| input `secret` | locked to another key, other tags, SIG_INPUTS, a locktime or refund, multisig | parsed strictly; `data` must be our key, tags exactly `[["sigflag","SIG_ALL"]]`; refused otherwise |
| input `secret` | two inputs with the same secret | refused |
| input `secret` | a JSON the mint reads differently (escapes, duplicate keys) | strict subset, escapes refused, duplicates refused |
| input `C` | wrong point | not our check: the mint refuses the swap, nothing leaves |
| output `amount` | inflated or deflated | signed in the message; the mint's balance rule binds the total |
| output `B_` | swapped after signing | signed in the message; a changed B_ fails the signature at the mint |
| output `change` | claimed on an output that is not ours | the signer rebuilds the secret, recomputes B_ and refuses a mismatch |
| output `change` | omitted on a real change output | counted as leaving; the screen shows more leaving, never less |
| change `counter` | a key that is ours but hardened or out of range | refused |
| change `nonce` | reused from an input or another change output | refused: the mint would accept the output but its secret is already spent, so the change could never be redeemed |
| change `r` | zero, or not a scalar | refused: zero would show the mint the unblinded point |
| `mint` | lies | shown as the phone's words, and the screen says so |
| `unit` | `msat` or any smaller unit, to shrink the amounts on screen | refused: only `sat` is accepted, since the same signature moves the same proofs whatever the label says |
| the session | review, lock, unlock the same seed, sign | refused: a review belongs to the session it was shown in, and any lock or unlock ends it |
| the reply | replayed on another swap | the signature covers every input and output; any other swap fails at the mint |

What it cannot prevent:

- The outputs without `change` go to whoever built their blinded messages,
  which is the coordinator. The signer shows how much that is and nothing
  more, as a PSBT signer cannot know who an address belongs to.
- The signer stops theft, not destruction. The coordinator holds every proof
  and every change blinding factor, so it can always throw ecash away. One
  way it can do that through a signed swap: give a change output a nonce
  whose secret was already spent. The signer proves the change is locked to
  it, not that the secret is fresh. Unlike deleting the page's storage, such
  a swap also spends the inputs, so a separate backup of them is lost too.
  It gains the coordinator nothing it can redeem. Version 1 does not prevent
  it; a later version could, by having the signer pick change nonces itself
  and return them in the reply.
- The proofs a keyset of another unit (usd, eur) issued could be labelled
  `sat`, since the signer never sees a keyset. The vault page only ever holds
  `sat`; another coordinator must not send other units under version 1.
- Fees: "leaving" counts the outputs that do not come back, not the mint's
  input fee, because input amounts are not in the request. The fee is what
  the inputs are worth minus all the outputs, and the mint keeps it.
- Totals of 16 digits or more (over 10,000,000 bitcoin in sats) can be cut
  short on the smallest screens in some languages, with an ellipsis. The
  signer does not check totals against any limit; this is an accepted
  display limit, since no mint holds anything near that amount.
- The mint holds the bitcoin behind all of it, so none of this protects
  against the mint.

## Every public way to get a signature

One: review then sign, through the module's two public calls. Review parses
and checks and keeps the checked request; sign signs only that kept request,
never bytes handed to it. There is no public call that signs a hash.

## Left out of version 1

Melt (paying a Lightning invoice): the quote id is signed, but the invoice is
not, so the signer could not show what is paid. Multisig and locktime inputs.
NUT-28 blinded keys. A larger request than one transport buffer.
