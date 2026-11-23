// The demos the Docs page plays on the live device, as data.
//
// docs/live.js reads this list and builds one tab per entry. A demo shows on
// the Docs page's first topic (#tour) unless it names another host. Adding, changing
// or dropping a demo is an edit here and nowhere else.
//
// A demo is a list of steps. Each step has the words shown beside the device
// and the things the page's finger does while those words are lit:
//
//   ["wait", ms]                 let the device run
//   ["tap", x, y]                press and lift at a point
//   ["drag", x0, y, x1, ms]      press, travel, lift (the slide bars)
//   ["kiss"]                     draw K, I, S, S, stroke by stroke
//   ["signer"]                   load the published test words
//   ["feed", name]               hand the camera the QR text in data[name]
//   ["until", "scanning", ms]    wait for the camera screen, or give up
//   ["rolls", digits]            tap those faces on the dice screen
//   ["type", text]               type on the device's own keyboard
//   ["stroke", [[x, y], ...]]    draw one stroke through those points
//   ["taps", n]                  n taps in the tap box, in an uneven rhythm
//
// "prep" runs before the device is shown, to reach the demo's first screen.
// Points are in the screen's own 800 by 480 space and belong to the firmware
// build in docs/sim. They hold until that build changes; after a rebuild, open
// guide.html?selftest#tour, which plays every demo and shows where each
// one ended.
//
// See it work keeps to the five things every owner does, in the order they
// do them. Anything more belongs in the topic that explains it.
//
// The keys are the published test words every bitcoin library is tested
// against (fingerprint 73C5DA0A), on testnet. Nothing here is worth anything.
// One underline under the word on the DRAW IT screen, sampled the way a finger
// is: 21 points, 14 points apart.
var UNDERLINE = [];
for (var u = 0; u <= 20; u++) UNDERLINE.push([260 + u * 14, 262]);

window.KISS_TOURS = [
  {
    // The Install page: the first minute after flashing, on a fresh device.
    id: "firstrun",
    host: "install-hero",
    scene: "sunset",
    tab: "First run",
    title: "What you will see next",
    poster: "media/game-menu.png",
    prep: [["wait", 1800]],
    steps: [
      { h: "FRUIT ISLAND", p: "A real game. Nothing says signer.",
        frame: "media/game-menu.png", run: [["wait", 2600]] },
      { h: "Draw K, I, S, S", p: "Four letters, lifting between each.",
        run: [["kiss"], ["wait", 1600]] },
      { h: "Set it up", p: "New seed words, or restore yours.",
        run: [["wait", 2800]] },
      { h: "Choose storage", p: "The Docs walk you through the rest.",
        frame: "media/setup-storage.png", run: [["tap", 300, 141], ["wait", 3200]] }
    ]
  },
  {
    // How it works: the topic's own four steps, performed.
    id: "flow",
    host: "how",
    tab: "Flow",
    title: "The four steps, on the device",
    poster: "media/export-descriptor.png",
    data: { pay: "cHNidP8BAHECAAAAAaqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqAAAAAAD9////AmDqAAAAAAAAFgAUERERERERERERERERERERERERERFYmAAAAAAAABYAFC80qhzwClOwVaKRoDp9RfCmmItSAAAAAAABAR+ghgEAAAAAABYAFNDEo+8J6Ze26Z45flGP4+QaEYyhIgYC56slN7XUnpcDCargbp5J82zhyf671E7I4NHMoLT5wxkYc8XaClQAAIABAACAAAAAgAAAAAAAAAAAAAAiAgNdSezNVNAJnkNnYnfHptRiXWEdqIpd9Jv5UXp3kad3pRhzxdoKVAAAgAEAAIAAAACAAQAAAAAAAAAA" },
    prep: [["signer"], ["wait", 2400]],
    steps: [
      { h: "Pair", p: "Your public keys, as a QR. Watch only.",
        frame: "media/export-descriptor.png", run: [["wait", 800], ["tap", 490, 240], ["wait", 4400]] },
      { h: "Build it there", p: "The coordinator makes it. The signer waits.",
        frame: "media/qr-scan.png",
        run: [["tap", 700, 436], ["wait", 1700], ["tap", 130, 240], ["wait", 1600], ["tap", 160, 412], ["until", "scanning", 3000], ["wait", 900], ["wait", 1300]] },
      { h: "Check and sign here", p: "Every amount worked out here. Slide to sign.",
        frame: "media/sign-verify.png",
        run: [["feed", "pay"], ["wait", 4200], ["drag", 70, 432, 372, 1300], ["wait", 2600]] },
      { h: "Send it there", p: "The signed QR goes back to broadcast.",
        run: [["wait", 6000]] }
    ],
    probe: { x: 58, y: 247, is: [245, 245, 250] }
  },
  {
    id: "seed",
    tab: "1 Seed words",
    title: "Make your seed words",
    poster: "media/game-menu.png",
    prep: [["wait", 1800]],
    steps: [
      { h: "Draw K, I, S, S", p: "On a new signer it opens set up.",
        frame: "media/game-menu.png", run: [["wait", 1200], ["kiss"], ["wait", 1800]] },
      { h: "New seed words", p: "Or restore the ones you have.",
        run: [["tap", 300, 141], ["wait", 2400]] },
      { h: "Choose storage", p: "Where they live. Tab 2 explains.",
        frame: "media/setup-storage.png", run: [["wait", 1400], ["tap", 300, 156], ["wait", 2200]] },
      { h: "Camera and taps", p: "Aim at gravel or leaves. Here the page fills it in.",
        run: [["tap", 300, 141], ["wait", 3600]] },
      { h: "Your taps", p: "64 taps, any rhythm.",
        run: [["tap", 120, 430], ["wait", 1400], ["taps", 66], ["wait", 800]] },
      { h: "Four sources, mixed", p: "No single one decides. Dice or coins work too.",
        run: [["wait", 4200]] },
      { h: "Write them down", p: "Paper only. These ones are public.",
        run: [["tap", 620, 430], ["wait", 3400]] },
      { h: "It checks your paper", p: "Three words, from your copy.",
        run: [["tap", 620, 430], ["wait", 3000]] }
    ]
  },
  {
    id: "storage",
    tab: "2 Storage",
    title: "Where your seed words live",
    poster: "media/setup-storage.png",
    prep: [["signer"], ["wait", 2400]],
    steps: [
      { h: "Settings, Backup", p: "It tracks whether you checked your paper.",
        run: [["wait", 800], ["tap", 670, 240], ["wait", 1500], ["tap", 361, 82], ["wait", 2400]] },
      { h: "Three places", p: "On the signer, on a locked card, or nowhere.",
        frame: "media/setup-storage.png", run: [["tap", 400, 270], ["wait", 4200]] }
    ]
  },
  {
    id: "backup",
    tab: "3 Backup",
    title: "Back it up",
    poster: "media/signer-home.png",
    prep: [["signer"], ["wait", 2400]],
    steps: [
      { h: "Paper first", p: "CHECK MY COPY compares it, word by word.",
        run: [["wait", 800], ["tap", 670, 240], ["wait", 1500], ["tap", 361, 82], ["wait", 1300], ["tap", 400, 175], ["wait", 2600]] },
      { h: "Then encrypted", p: "A QR or file that only your password opens.",
        run: [["tap", 450, 84], ["wait", 1600], ["tap", 400, 170], ["wait", 2600]] },
      { h: "satoshi: refused", p: "Too easy to guess.",
        run: [["drag", 70, 432, 372, 1300], ["wait", 1400], ["type", "satoshi"], ["wait", 600], ["tap", 734, 437], ["wait", 3000],
              ["tap", 411, 381], ["wait", 1000]] },
      { h: "Tq8#mV2!xR5k: accepted", p: "12 random characters. Typed twice.",
        run: [["clear", 8], ["type", "Tq8#mV2!xR5k"], ["wait", 900], ["tap", 734, 437], ["wait", 1400],
              ["type", "Tq8#mV2!xR5k"], ["wait", 600], ["tap", 734, 437], ["wait", 2400]] },
      { h: "Your encrypted QR", p: "Keep the password somewhere else.",
        run: [["wait", 4000]] }
    ]
  },
  {
    id: "swipe",
    tab: "4 Your swipe",
    title: "Your swipe, on top of the decoy",
    poster: "media/signer-home.png",
    prep: [["signer"], ["wait", 2400]],
    steps: [
      { h: "Settings, Security, Duress", p: "K, I, S, S alone will open a decoy.",
        run: [["wait", 800], ["tap", 670, 240], ["wait", 1500], ["tap", 233, 82], ["wait", 1300], ["tap", 400, 150], ["wait", 2600]] },
      { h: "A passphrase", p: "Makes the real keys, with their own fingerprint.",
        run: [["tap", 120, 430], ["wait", 2000], ["tap", 120, 430], ["wait", 2600], ["tap", 688, 430], ["wait", 1400],
              ["type", "Wn4!pZ7@cL3h"], ["wait", 500], ["tap", 734, 437], ["wait", 1400],
              ["type", "Wn4!pZ7@cL3h"], ["wait", 500], ["tap", 734, 437], ["wait", 3200]] },
      { h: "The decoy", p: "Your seed words alone. Keep a little in it.",
        run: [["tap", 157, 434], ["wait", 2200], ["tap", 660, 434], ["wait", 2400], ["tap", 150, 434], ["wait", 3000]] },
      { h: "Pick a swipe", p: "Drawn after the four letters.",
        run: [["tap", 190, 434], ["wait", 2000], ["tap", 650, 434], ["wait", 2400], ["tap", 130, 158], ["wait", 1800]] },
      { h: "Draw it twice", p: "K, I, S, S plus your swipe opens the real keys.",
        run: [["stroke", UNDERLINE], ["wait", 1600], ["stroke", UNDERLINE], ["wait", 3400]] }
    ]
  },
  {
    id: "pair",
    tab: "5 Pair",
    title: "Pair your coordinator",
    poster: "media/export-descriptor.png",
    prep: [["signer"], ["wait", 2400]],
    steps: [
      { h: "Keys", p: "Public keys only. It can watch, never spend.",
        frame: "media/export-descriptor.png", run: [["wait", 800], ["tap", 490, 240], ["wait", 3600]] },
      { h: "Steps for your app", p: "Sparrow, BlueWallet, or any that reads a descriptor.",
        run: [["tap", 560, 84], ["wait", 3200], ["tap", 380, 84], ["wait", 2400]] }
    ]
  },
  {
    id: "receive",
    tab: "6 Receive",
    title: "Receive, and check the address",
    poster: "media/signer-home.png",
    data: {
      own: "tb1q6rz28mcfaxtmd6v789l9rrlrusdprr9pqcpvkl",
      other: "tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx"
    },
    prep: [["signer"], ["wait", 2400]],
    steps: [
      { h: "A fresh address", p: "A new one each time.",
        run: [["wait", 800], ["tap", 310, 240], ["wait", 2600], ["tap", 410, 428], ["wait", 2400]] },
      { h: "Check one", p: "Scan what your coordinator shows: yours.",
        run: [["tap", 90, 428], ["until", "scanning", 3000], ["wait", 900], ["feed", "own"], ["wait", 3400]] },
      { h: "Catch a swap", p: "Not yours: malware changed it.",
        run: [["tap", 90, 430], ["until", "scanning", 3000], ["wait", 900], ["feed", "other"], ["wait", 3600]] }
    ]
  },
  {
    id: "sign",
    tab: "7 Sign",
    title: "Nothing is signed unseen",
    poster: "media/signer-home.png",
    data: {
      // 60 000 sats on testnet, the first fixture sim/mk_sd_psbts.sh writes,
      // as the base64 text a coordinator puts in a QR.
      pay: "cHNidP8BAHECAAAAAaqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqAAAAAAD9////AmDqAAAAAAAAFgAUERERERERERERERERERERERERERFYmAAAAAAAABYAFC80qhzwClOwVaKRoDp9RfCmmItSAAAAAAABAR+ghgEAAAAAABYAFNDEo+8J6Ze26Z45flGP4+QaEYyhIgYC56slN7XUnpcDCargbp5J82zhyf671E7I4NHMoLT5wxkYc8XaClQAAIABAACAAAAAgAAAAAAAAAAAAAAiAgNdSezNVNAJnkNnYnfHptRiXWEdqIpd9Jv5UXp3kad3pRhzxdoKVAAAgAEAAIAAAACAAQAAAAAAAAAA"
    },
    prep: [["signer"], ["wait", 2400]],
    steps: [
      { h: "Open Sign", p: "The camera is the only way in.",
        frame: "media/signer-home.png",
        run: [["wait", 900], ["tap", 130, 240], ["wait", 1600], ["tap", 160, 412],
              ["until", "scanning", 3000], ["wait", 1100]] },
      { h: "Scan it", p: "60 000 sats to pay.",
        frame: "media/qr-scan.png",
        run: [["feed", "pay"], ["wait", 1800]] },
      { h: "Where the money goes", p: "Worked out here, not taken on trust.",
        frame: "media/sign-verify.png",
        run: [["wait", 4000]] },
      { h: "Slide to sign", p: "The signed QR goes back.",
        run: [["drag", 70, 432, 372, 1300], ["wait", 6500]] }
    ],
    // Passes when the white QR card is on screen.
    probe: { x: 58, y: 247, is: [245, 245, 250] }
  },,
  {
    id: "checks",
    tab: "8 Checks",
    title: "Nothing is signed unseen",
    poster: "media/sign-verify.png",
    data: { dust: "cHNidP8BAP0VAQIAAAAF/y8NgWp6zEeeA6JJ6iS1ba6S2xkBH57Xkk0cNPmNyXEAAAAAAP3///9T71ZkJOPjrT1MA3TIPUJO6xUO7YtJDOwQXhrxt6nu4AAAAAAA/f///yczMi+7j4eC9ZKH5WB3mklkrHBIdcgbwVY/wQuX5rkJAAAAAAD9////CLQOJcgh4ct+Vla7pUCvEEYAY8Qh/Pk7hyKVJZmDUFIAAAAAAP3///8U80VFbwuqo+jcXK5wYoXntoJzDXZ7XvMPEmW/rWPfzAAAAAAA/f///wIQJwAAAAAAABYAFBERERERERERERERERERERERERERyAAAAAAAAAAWABQvNKoc8ApTsFWikaA6fUXwppiLUgAAAAAAAQBSAgAAAAGwsLCwsLCwsLCwsLCwsLCwsLCwsLCwsLCwsLCwsLCwsAAAAAAA/////wG4CwAAAAAAABYAFNDEo+8J6Ze26Z45flGP4+QaEYyhAAAAAAEBH7gLAAAAAAAAFgAU0MSj7wnpl7bpnjl+UY/j5BoRjKEiBgLnqyU3tdSelwMJquBunknzbOHJ/rvUTsjg0cygtPnDGRhzxdoKVAAAgAEAAIAAAACAAAAAAAAAAAAAAQBSAgAAAAGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsQAAAAAA/////wG4CwAAAAAAABYAFG+gFlAKPGpzfrsmDi3cp4upI0VYAAAAAAEBH7gLAAAAAAAAFgAUb6AWUAo8anN+uyYOLdyni6kjRVgiBgPu7SBaaQIv7UpioCRX82mbGcBr90v4AazG2a6EvBap4RhzxdoKVAAAgAEAAIAAAACAAAAAAAEAAAAAAQBSAgAAAAGysrKysrKysrKysrKysrKysrKysrKysrKysrKysrKysgAAAAAA/////wG4CwAAAAAAABYAFDNJJOr0boBuhrNTehL4FZUDDXOnAAAAAAEBH7gLAAAAAAAAFgAUM0kk6vRugG6Gs1N6EvgVlQMNc6ciBgIzkZPDTNjssh69SK9k6tcdeCE0cNYdcnT5Mkida6Ib0xhzxdoKVAAAgAEAAIAAAACAAAAAAAIAAAAAAQBSAgAAAAGzs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7OzswAAAAAA/////wG4CwAAAAAAABYAFCTCiGndDq5eMJ6TzfzDLlOOoE3xAAAAAAEBH7gLAAAAAAAAFgAUJMKIad0Orl4wnpPN/MMuU46gTfEiBgKxVx9wwNUrqNSRr6DpiR+Dy8VYx00SUJMH7dGIq/gA9BhzxdoKVAAAgAEAAIAAAACAAAAAAAMAAAAAAQBSAgAAAAG0tLS0tLS0tLS0tLS0tLS0tLS0tLS0tLS0tLS0tLS0tAAAAAAA/////wG4CwAAAAAAABYAFNe8X0fue7xdIWsJKKSoupA720BPAAAAAAEBH7gLAAAAAAAAFgAU17xfR+57vF0hawkopKi6kDvbQE8iBgO7XbISGS1bQoxdtyarohQm0KY7ekU7AQTyOYMmvKQ/whhzxdoKVAAAgAEAAIAAAACAAAAAAAQAAAAAACICA11J7M1U0AmeQ2did8em1GJdYR2oil30m/lReneRp3elGHPF2gpUAACAAQAAgAAAAIABAAAAAAAAAAA=", lie: "cHNidP8BAHECAAAAAeDDtUoVUZYPW7etQGW+CmmX97sD+YbQZh1yBA5+COBcAAAAAAD9////AmDqAAAAAAAAFgAUERERERERERERERERERERERERERFYmAAAAAAAABYAFC80qhzwClOwVaKRoDp9RfCmmItSAAAAAAABAFICAAAAAbCwsLCwsLCwsLCwsLCwsLCwsLCwsLCwsLCwsLCwsLCwAAAAAAD/////AaCGAQAAAAAAFgAU0MSj7wnpl7bpnjl+UY/j5BoRjKEAAAAAAQEfoYYBAAAAAAAWABTQxKPvCemXtumeOX5Rj+PkGhGMoSIGAuerJTe11J6XAwmq4G6eSfNs4cn+u9ROyODRzKC0+cMZGHPF2gpUAACAAQAAgAAAAIAAAAAAAAAAAAAAIgIDXUnszVTQCZ5DZ2J3x6bUYl1hHaiKXfSb+VF6d5Gnd6UYc8XaClQAAIABAACAAAAAgAEAAAAAAAAAAA==", net: "cHNidP8BAHECAAAAAaqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqAAAAAAD9////AmDqAAAAAAAAFgAUERERERERERERERERERERERERERFYmAAAAAAAABYAFD40mF3Kb93J+zaZQOTH2OKHP1KcAAAAAAABAR+ghgEAAAAAABYAFMDOvNbD08qMddxexi6+VTMO+RDiIgYDMNVP0N1CCm5fjTYk9fNILK41D3nV8HU79b7vnC2RrzwYc8XaClQAAIAAAACAAAAAgAAAAAAAAAAAAAAiAgMCUySIjkKauOPbrx94AmSLnNAem0GEhcX6TBubVwDhphhzxdoKVAAAgAAAAIAAAACAAQAAAAAAAAAA" },
    prep: [["signer"], ["wait", 2400]],
    steps: [
      { h: "Spending tiny coins", p: "Fee is half the payment. Four cautions.",
        run: [["wait", 800], ["tap", 130, 240], ["wait", 1600], ["tap", 160, 412], ["until", "scanning", 3000], ["wait", 900], ["feed", "dust"], ["wait", 3600], ["tap", 690, 304], ["wait", 3600]] },
      { h: "A coordinator lies", p: "About what a coin holds: stop.",
        run: [["tap", 730, 416], ["wait", 1400], ["tap", 745, 430], ["wait", 1800], ["tap", 160, 412],
              ["until", "scanning", 3000], ["wait", 900], ["feed", "lie"], ["wait", 3800]] },
      { h: "The wrong network", p: "Mainnet on testnet keys: stop.",
        run: [["tap", 745, 430], ["wait", 1800], ["tap", 160, 412], ["until", "scanning", 3000], ["wait", 900],
              ["feed", "net"], ["wait", 3800]] }
    ]
  }
];
