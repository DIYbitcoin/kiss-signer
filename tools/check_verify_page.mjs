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
  "c6f9982e11f767f5e17fb8321323a2609c9d69dd787d9b69039612564ef097aa";
const VEC_WORDS =
  "shoulder smoke argue catalog island wife magnet warfare craft october " +
  "trigger scorpion six reject invest autumn opinion elite tortoise caution " +
  "gossip joke gadget execute";

// The page hashes the FILE the device writes: every second pixel of every
// second row of the raw pattern frame, two bytes per pixel. Built here the
// same way kiss_proof.c builds it, so the two vectors are one vector.
const RAW_W = 1288, RAW_H = 728;
const frame = new Uint8Array(ctx.FRAME_BYTES);
if (frame.length !== 644 * 364 * 2) fail(`FRAME_BYTES is ${frame.length}`);
let o = 0;
for (let y = 0; y < 364; y++) {
  const row = (y * 2) * RAW_W * 2;
  for (let x = 0; x < 644; x++) {
    const p = row + x * 4;
    frame[o++] = (p * 31 + 7) & 0xff;
    frame[o++] = ((p + 1) * 31 + 7) & 0xff;
  }
}

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

// The page is stateless now: no baked claim, no slot, nothing per-run. A
// claim mechanism coming back is how the torn-pair failure returns, so its
// absence is asserted as hard as its presence used to be.
if (typeof ctx.KISS_CLAIM !== "undefined" || typeof ctx.bakedClaim !== "undefined")
  fail("the page grew a baked claim again; it must stay stateless");

if (ctx.WORDS.length !== 2048 || ctx.WORDS[0] !== "abandon" || ctx.WORDS[2047] !== "zoo")
  fail("embedded wordlist is not the standard 2048");

console.log("verify page vector: PASS (hash, 24 words, #h= parser)");

function fail(msg) {
  console.error("verify page vector: FAIL — " + msg);
  process.exit(1);
}
