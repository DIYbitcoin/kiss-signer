// Pins docs/verify.html's JS to the same vector sim/test_proof.c pins the
// firmware to, so C, Python and the page can never drift apart:
//   buf[i] = (i*31+7) & 0xFF over the 1,875,328 frame bytes.
// The page's script body is written DOM-free above a typeof-document guard,
// which is what lets this file run it under Node with no browser.
//
//   node tools/check_verify_page.mjs
//
// Exits 0 silently-ish on success, 1 with the first mismatch otherwise.
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const page = readFileSync(join(root, "docs", "verify.html"), "utf8");

const m = /<script>([\s\S]*)<\/script>/.exec(page);
if (!m) fail("no <script> block in docs/verify.html");

const ctx = {};
vm.createContext(ctx);
vm.runInContext(m[1], ctx, { filename: "verify.html <script>" });

const VEC_HASH =
  "628e71b5036701e7a509d4708178d88e2cb1f49af06c0b63cfd7a1e9759fb704";
const VEC_WORDS =
  "glad inform hood almost hybrid video neither dentist identify armed " +
  "curtain brisk slam where hint assault arena bunker vote duck nuclear " +
  "sound swing machine";

const frame = new Uint8Array(ctx.FRAME_BYTES);
if (frame.length !== 1875328) fail(`FRAME_BYTES is ${frame.length}`);
for (let i = 0; i < frame.length; i++) frame[i] = (i * 31 + 7) & 0xff;

const hash = ctx.sha256(frame);
const hex = ctx.toHex(hash);
if (hex !== VEC_HASH) fail(`sha256 mismatch\n  got  ${hex}\n  want ${VEC_HASH}`);

const words = ctx.bip39Words(hash).join(" ");
if (words !== VEC_WORDS) fail(`words mismatch\n  got  ${words}\n  want ${VEC_WORDS}`);

// The tiles the page draws are bip39Slices, so the bits a reader is shown have
// to spell the word printed beside them. Nothing on screen can reveal a slice
// that disagrees with its own word, and only this check says so.
const slices = ctx.bip39Slices(hash);
if (slices.length !== 24) fail(`bip39Slices returned ${slices.length} slices`);
let csBits = 0;
slices.forEach((s, i) => {
  if (s.bits.length !== 11) fail(`slice ${i} has ${s.bits.length} bits`);
  const spelled = s.bits.reduce((acc, b) => (acc << 1) | (b.on ? 1 : 0), 0);
  if (spelled !== s.index) fail(`slice ${i} bits spell ${spelled}, index says ${s.index}`);
  if (ctx.WORDS[s.index] !== s.word) fail(`slice ${i} word is not index ${s.index}`);
  if (s.word !== VEC_WORDS.split(" ")[i]) fail(`slice ${i} disagrees with bip39Words`);
  s.bits.forEach((b, j) => {
    if (b.cs !== (i * 11 + j >= 256)) fail(`slice ${i} bit ${j} mislabels its source`);
    if (b.cs) csBits++;
  });
});
if (csBits !== 8) fail(`${csBits} bits marked checksum, want 8`);

if (ctx.claimedHash("#h=" + VEC_HASH.toUpperCase()) !== VEC_HASH)
  fail("claimedHash does not normalize case");
if (ctx.claimedHash("#h=" + VEC_HASH.slice(1)) !== null)
  fail("claimedHash accepted 63 hex chars");
if (ctx.claimedHash("") !== null) fail("claimedHash accepted an empty fragment");

// The slot kiss_proof.c writes over for the card's self verifying copy.
if (ctx.KISS_CLAIM !== "-".repeat(64))
  fail("the shipped page does not carry an empty 64 dash claim slot");
if (ctx.bakedClaim(ctx.KISS_CLAIM) !== null)
  fail("an unwritten slot must read as no claim");
if (ctx.bakedClaim(VEC_HASH.toUpperCase()) !== VEC_HASH)
  fail("bakedClaim does not accept the hash the device writes into the slot");
if (ctx.bakedClaim(undefined) !== null || ctx.bakedClaim("nope") !== null)
  fail("bakedClaim accepted a missing or malformed claim");

if (ctx.WORDS.length !== 2048 || ctx.WORDS[0] !== "abandon" || ctx.WORDS[2047] !== "zoo")
  fail("embedded wordlist is not the standard 2048");

console.log("verify page vector: PASS (hash, 24 words, #h= parser)");

function fail(msg) {
  console.error("verify page vector: FAIL — " + msg);
  process.exit(1);
}
