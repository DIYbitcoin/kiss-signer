// KISS ecash vault: a coordinator for docs/specs/ecash-spend-request.md.
//
// What it keeps is locked to the signer's keys (NUT-11 P2PK, SIG_ALL, keys
// from the signer's NUT-13 export), so a copy of this page's storage cannot
// spend it. Bearer ecash passes through only on its way somewhere: a pasted
// token until its swap into locked ecash finishes, and a payment the signer
// approved until the owner has handed it over.
//
// Four rules hold everywhere below, whatever the mint, the network or a
// sender does:
//  1. Nothing is deleted that might still be worth something. A record of
//     an operation sent to the mint leaves only when every output has a
//     checked signature and the proofs are saved in the same write. The
//     owner may set a record aside; it is still kept, and still picked up
//     if the mint later shows it finished.
//  2. Nothing is credited that the mint has not provably signed: a DLEQ
//     proof (NUT-12) with keys whose keyset id is rebuilt from the keys
//     themselves, on the page's own output or on a tip's own proof.
//  3. Nothing is credited twice: a proof is added only if it is not held,
//     set aside or already spent from this vault, checked inside the write.
//  4. One writer: one tab holds the vault (Web Locks), under a storage key
//     no older version of this page reads or writes, and every write is
//     tied to the pairing it was started for.
import { blindMessage, unblindSignature, pointFromHex, createP2PKsecret,
  getEncodedToken, getDecodedToken, hashToCurve, hasValidDleq, deriveKeysetId,
  PaymentRequest } from './vendor/cashu-cashu-ts/lib/cashu-ts.es.js';
import { SimplePool } from './vendor/nostr-tools/pool.js';
import { unwrapEvent } from './vendor/nostr-tools/nip59.js';
import { generateSecretKey, getPublicKey } from './vendor/nostr-tools/pure.js';
import { nprofileEncode } from './vendor/nostr-tools/nip19.js';
import { HDKey } from '@scure/bip32';
import { sha256 } from '@noble/hashes/sha2.js';
import { schnorr } from '@noble/curves/secp256k1.js';

const MINT = 'https://testnut.cashu.space';
const STORE = 'kiss-ecash-v2';
const OLD_STORE = 'kiss-ecash-v1';   // the earlier page's vault: never read, never touched
const FORGOTTEN = `${STORE}-forgotten:`;   // + xpub: the vault as it was when forgotten
const REQ_MAX = 4096;          // the signer's QR transport limit
const FRAME_CHARS = 200;       // base64 characters per animated frame
const SIGALL = [['sigflag', 'SIG_ALL']];
const KEEP_SENT = 200;         // handed over payment tokens kept to look at again
const TIMEOUT = 20000;         // a mint request with no answer by then is given up

const $ = (id) => document.getElementById(id);
const enc = (s) => new TextEncoder().encode(s);
const hex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
const sats = (n) => `${n} ${n === 1 ? 'sat' : 'sats'}`;
const unhex = (h) => Uint8Array.from(h.match(/../g) || [], (b) => parseInt(b, 16));
const rand32 = () => hex(crypto.getRandomValues(new Uint8Array(32)));
const tag = (s) => hex(sha256(enc(s))).slice(0, 32);
const yOf = (secret) => hashToCurve(enc(secret)).toHex(true);
const listTag = (secrets) => tag(JSON.stringify([...secrets].sort()));

// A refusal is final: the input itself is wrong, and retrying cannot help.
// Anything else (the mint or the network failing) may work the next time.
class Refusal extends Error {}
// The vault is in storage but cannot be read: nothing may replace or remove
// it, and the page offers its raw text instead.
class Unreadable extends Error {}

// ---- state ----
let st = null;
let owner = false;
// null only when there is no vault; a vault that is there but cannot be
// read, parsed, or has a list of the wrong shape is an error, never "no
// vault" and never repaired: nothing may write over it.
function load() {
  let raw;
  try { raw = localStorage.getItem(STORE); } catch (e) {
    throw new Unreadable(`this browser would not let the page read its storage (${e.message || e})`);
  }
  if (raw === null) return null;
  let v = null;
  try { v = JSON.parse(raw); } catch { /* below */ }
  if (wellFormed(v)) return v;
  throw Object.assign(new Unreadable('the vault in this browser\'s storage cannot be read'), { raw });
}
// The shape every version of the vault has kept: an xpub, and each list,
// when present, a list of the right kind of entries.
// strict: the vault about to be saved must have every list and field, so
// no action can save a vault the next load would refuse.
function wellFormed(v, strict) {
  if (!v || typeof v !== 'object' || Array.isArray(v) || typeof v.xpub !== 'string') return false;
  if (strict && (!LISTS.every((k) => Array.isArray(v[k])) || !Number.isSafeInteger(v.next) || typeof v.gen !== 'string' || !v.kpin)) return false;
  for (const k of LISTS) {
    if (v[k] === undefined) continue;
    if (!Array.isArray(v[k])) return false;
    const want = ['done', 'spent', 'sentKeys'].includes(k) ? 'string' : 'object';
    if (v[k].some((x) => typeof x !== want || x === null)) return false;
  }
  const isProof = (p) => typeof p.secret === 'string' && typeof p.C === 'string' && typeof p.id === 'string' && Number.isSafeInteger(p.amount) && p.amount > 0;
  if ((v.proofs || []).some((p) => !isProof(p) || !Number.isSafeInteger(p.counter))) return false;
  if ((v.aside || []).some((p) => !isProof(p) || !Number.isSafeInteger(p.counter))) return false;
  const isOut = (o) => o && typeof o.B_ === 'string' && typeof o.secret === 'string' && typeof o.r === 'string' && Number.isSafeInteger(o.amount);
  for (const k of ['pending', 'archive']) {
    for (const r of v[k] || []) {
      if (!['mint', 'bearer', 'spend'].includes(r.kind) || typeof r.id !== 'string' || typeof r.rid !== 'string') return false;
      if (!Array.isArray(r.outs) || !r.outs.length || !r.outs.every(isOut)) return false;
      if (r.prev !== undefined && (!Array.isArray(r.prev) || !r.prev.every((x) => Array.isArray(x) && x.every(isOut)))) return false;
      if (r.inputs !== undefined && (!Array.isArray(r.inputs) || !r.inputs.every(isProof))) return false;
      if (r.spends !== undefined && (!Array.isArray(r.spends) || r.spends.some((x) => typeof x !== 'string'))) return false;
      if (r.got !== undefined && (!r.got || typeof r.got !== 'object' || Array.isArray(r.got) || !Object.values(r.got).every((x) => x && isProof(x)))) return false;
    }
  }
  if ([...(v.outbox || []), ...(v.sent || [])].some((x) => typeof x.token !== 'string' || typeof x.id !== 'string')) return false;
  if (strict && [...v.outbox, ...v.sent].some((x) => typeof x.key !== 'string')) return false;
  if (v.jar !== undefined) {
    const j = v.jar;
    if (!j || typeof j !== 'object' || !Number.isSafeInteger(j.counter)) return false;
    if (j.nsk !== undefined && !/^[0-9a-f]{64}$/.test(j.nsk)) return false;
    if (j.seen !== undefined && (!Array.isArray(j.seen) || j.seen.some((x) => typeof x !== 'string'))) return false;
  }
  if (v.kpin && Object.values(v.kpin).some((x) => typeof x !== 'string')) return false;
  if (v.kpin !== undefined && (typeof v.kpin !== 'object' || v.kpin === null || Array.isArray(v.kpin))) return false;
  if (v.next !== undefined && !Number.isSafeInteger(v.next)) return false;
  return true;
}
const readRaw = (key) => { try { return localStorage.getItem(key); } catch { return null; } };
const parseVault = (raw) => { try { const v = JSON.parse(raw); return wellFormed(v) ? v : null; } catch { return null; } };
function save(v) {
  if (!wellFormed(v, true)) throw new Error('this page was about to save a vault in a shape it could not read back, so nothing was saved; reload the page');
  try { localStorage.setItem(STORE, JSON.stringify(v)); } catch (e) {
    throw new Error(`could not save the vault in this browser (${e.message || e}); reload the page before doing anything else`);
  }
}
// Every change: reread, refuse if the vault was forgotten or paired again
// since the change began (each pairing has its own gen), apply, write.
function commit(gen, fn) {
  const cur = load();
  if (!cur || cur.gen !== gen) throw new Refusal('this vault was forgotten or replaced; reload the page');
  const out = fn(cur);
  save(cur);
  st = cur;
  try { render(); } catch (e) { console.error(e); }
  return out;
}
window.addEventListener('storage', (e) => {
  if (e.key !== STORE || !owner) return;
  const was = st && st.gen;
  try { st = load(); } catch { st = null; }
  if (!st || st.gen !== was) { stopJar(); clearSignerViews(); }
  render();
});
// Lists that only grow while the vault lives: spent proofs (tags), finished
// operations and their output sets (tags) and handed over payments (keys)
// are identities, kept for good; sent keeps the last payment tokens to look
// at again.
const LISTS = ['proofs', 'pending', 'archive', 'aside', 'outbox', 'sent', 'sentKeys', 'done', 'spent'];
const fresh = (xpub, fp) => ({ xpub, fp, gen: rand32(), next: 0, kpin: {}, ...Object.fromEntries(LISTS.map((k) => [k, []])) });
const holds = (v) => v && ['proofs', 'pending', 'archive', 'aside', 'outbox'].some((k) => (v[k] || []).length);
const outsTag = (r) => 'o:' + listTag(r.outs.map((o) => o.B_));
// Bring a stored vault up to this version, in one write: lists that are
// missing are added, each proof is kept once across the balance and the
// held apart list, C is kept in the one spelling the signer reads, and a
// proof the signer would refuse is marked so it is never picked for a
// spend (its secret is kept as it is). Every proof in this vault was saved
// only after the mint's signature on it was checked. The earlier version of
// this page kept its test ecash under its own key; this page does not read
// it, and leaves it there untouched.
function tidy() {
  const cur = load();
  if (!cur) return;
  cur.gen = cur.gen || rand32();
  cur.next = Number.isSafeInteger(cur.next) ? cur.next : 0;
  for (const k of LISTS) cur[k] = cur[k] || [];
  cur.kpin = cur.kpin || {};
  const seen = new Set(), pubs = new Map();
  cur.proofs = cur.proofs.filter((p) => !seen.has(p.secret) && seen.add(p.secret));
  cur.aside = cur.aside.filter((p) => !seen.has(p.secret) && seen.add(p.secret));
  for (const p of [...cur.proofs, ...cur.aside]) {
    delete p.bad;
    delete p.unchecked;
    delete p.ok;
    try { p.C = pointFromHex(p.C).toHex(true); } catch { p.bad = true; continue; }
    if (!pubs.has(p.counter)) pubs.set(p.counter, pubAt(cur.xpub, p.counter));
    if (!signerLock(pubs.get(p.counter), p.secret)) p.bad = true;
  }
  for (const x of [...cur.outbox, ...cur.sent]) x.key = x.key || 'tok:' + tag(x.token);
  cur.sentKeys = cur.sentKeys.filter((k) => typeof k === 'string');
  save(cur);
  st = cur;
}

// ---- mint ----
// Mint errors carry the mint's code (0 when it gave none). An error with no
// code at all means no answer came back: the request may or may not have
// reached the mint. A request with no answer within TIMEOUT is given up the
// same way, so nothing waits on a stalled mint for ever; any record it
// belongs to stays and is tried again.
async function api(path, body, wait = TIMEOUT) {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), Math.max(1000, wait));
  let r, t;
  try {
    r = await fetch(MINT + path, { signal: ctl.signal, ...(body ? { method: 'POST',
      headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) } : {}) });
    t = await r.text();
  } catch (e) {
    throw new Error(ctl.signal.aborted ? `the mint did not answer in time` : (e.message || String(e)));
  } finally { clearTimeout(timer); }
  let j = null;
  try { j = JSON.parse(t); } catch { /* some mints answer errors in plain text */ }
  if (!r.ok) {
    const e = new Error(`mint said: ${(j && (j.detail || j.error)) || t.slice(0, 200) || r.status}`);
    e.code = j && Number.isInteger(j.code) ? j.code : 0;
    throw e;
  }
  if (!j || typeof j !== 'object') throw new Error('the mint gave an answer that is not JSON');
  return j;
}
// The mint's sat keysets and its active one. Refreshed every ten minutes, so
// a page left open does not keep making outputs for a retired keyset, and
// when a proof names a keyset the list does not have (at most every half
// minute).
let keyset = null, keysetAt = 0;
async function ks(refresh) {
  const age = Date.now() - keysetAt;
  if (keyset && age < 600000 && !(refresh && age > 30000)) return keyset;
  if (!keyset) {
    // The mint must say plainly that it supports what this page relies on:
    // spent checks (NUT-07), restore (NUT-09), signer locks (NUT-11) and
    // proofs of its signatures (NUT-12). Anything less is retried later.
    const info = await api('/v1/info');
    const has = (n) => info.nuts && info.nuts[n] && info.nuts[n].supported === true;
    if (!['7', '9', '11', '12'].every(has))
      throw new Error('the mint does not say it supports spent checks, restore, signer locks and signature proofs (NUT-07, 09, 11, 12); this page needs all four and tries again later');
  }
  const all = (await api('/v1/keysets')).keysets;
  if (!Array.isArray(all)) throw new Error('the mint gave a malformed keyset list');
  // Only rows with a well formed id are used at all; a malformed row is the
  // mint's problem and never blamed on a token or a tip.
  const okId = (id) => typeof id === 'string' && /^(00[0-9a-f]{14}|01[0-9a-f]{64})$/.test(id);
  const rows = all.filter((x) => x && typeof x === 'object' && okId(x.id) && typeof x.unit === 'string');
  const list = {};
  for (const x of rows) if (x.unit === 'sat') list[x.id] = x;
  const act = Object.values(list).find((x) => x.active === true);
  if (!act) throw new Error('the mint lists no active sat keyset');
  const k = await keysFor(act.id, list);
  keyset = { id: act.id, keys: k.keys, list, ids: rows.map((x) => x.id) };
  keysetAt = Date.now();
  return keyset;
}
// The keys of one keyset, active or not, kept only once its id has been
// rebuilt from them (with the unit, fee and expiry it commits to): a mint
// cannot hand over other keys under a known id. The id must be on the
// mint's sat list, the amounts must be whole numbers written plainly, and
// no key may serve two amounts. A version 00 id commits to the keys in
// amount order but not to the amounts, so for 00 the amounts must be
// exactly 1, 2, 4 and so on, the one labelling that order allows. The
// vault also remembers each keyset's keys the first time it sees them and
// refuses a later different set under the same id.
const verified = new Map();
const amountOrder = ([a], [b]) => (BigInt(a) < BigInt(b) ? -1 : BigInt(a) > BigInt(b) ? 1 : 0);
async function keysFor(id, list = keyset && keyset.list) {
  if (verified.has(id)) return pinned(id, verified.get(id), list && list[id]);
  if (typeof id !== 'string' || !/^(00[0-9a-f]{14}|01[0-9a-f]{64})$/.test(id)) throw new Error(`keyset ${id} is not one this page can check`);
  if (!list || !list[id]) throw new Error(`keyset ${id} is not on the mint's list of sat keysets`);
  const k = ((await api(`/v1/keys/${id}`)).keysets || []).find((x) => x && x.id === id);
  if (!k || k.unit !== 'sat' || !k.keys || typeof k.keys !== 'object') throw new Error(`the mint has no sat keys for keyset ${id}`);
  // Amounts are read as decimal strings (up to 2^64): the page only limits
  // the amounts it actually uses, not the keyset as a whole.
  const amts = Object.keys(k.keys);
  if (!amts.length || amts.some((a) => !/^[1-9][0-9]{0,19}$/.test(a) || BigInt(a) > 2n ** 64n))
    throw new Error(`the mint's keyset ${id} has amounts this page cannot read`);
  let pts;
  try { pts = amts.map((a) => pointFromHex(k.keys[a]).toHex(true)); } catch { throw new Error(`the mint's keyset ${id} holds a key that is not a curve point`); }
  if (new Set(pts).size !== amts.length) throw new Error(`the mint's keyset ${id} uses one key for two amounts`);
  if (id.startsWith('00')) {
    const sorted = amts.map(BigInt).sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
    if (sorted.some((a, i) => a !== 2n ** BigInt(i))) throw new Error(`the mint's keyset ${id} has amounts its id cannot vouch for`);
  }
  const meta = list[id];
  const fees = [...new Set([k.input_fee_ppk, meta.input_fee_ppk, 0])].filter((x) => x !== undefined);
  const exps = [...new Set([k.final_expiry, meta.final_expiry, undefined])];
  let found = null;
  for (const ppk of fees) for (const expiry of exps) {
    if (found) break;
    try {
      if (deriveKeysetId(k.keys, { unit: 'sat', input_fee_ppk: ppk || undefined, expiry: expiry || undefined, versionByte: parseInt(id.slice(0, 2), 16) }) === id)
        found = { ppk: Number(ppk) || 0, expiry: Number(expiry) || 0 };
    } catch { /* try the next */ }
  }
  if (!found) throw new Error(`the mint's keys for keyset ${id} do not match its id`);
  const pin = tag(JSON.stringify(amts.map((a, i) => [a, pts[i]]).sort(amountOrder)));
  const e = { keys: k.keys, pin, ...found };
  verified.set(id, e);
  return pinned(id, e, meta);
}
// Every use of a keyset's keys, cached or not: they must match the keys
// this vault first saw under that id. A 00 keyset's fee and expiry (which
// its id does not commit to) follow the mint's current list.
function pinned(id, e, meta) {
  if (st && st.kpin && st.kpin[id] && st.kpin[id] !== e.pin) throw new Error(`the mint's keys for keyset ${id} changed since this vault first saw them`);
  if (st && owner && st.kpin && !st.kpin[id]) { try { commit(st.gen, (s) => { s.kpin[id] = s.kpin[id] || e.pin; }); } catch { /* remembered next time */ } }
  if (id.startsWith('00') && meta) {
    e.ppk = Number(meta.input_fee_ppk) || 0;
    e.expiry = Number(meta.final_expiry) || 0;
  }
  return e;
}
const expired = (e) => e.expiry > 0 && e.expiry * 1000 <= Date.now();
// Keys for a proof's keyset. One not on the mint's list refreshes the list;
// it is refused only when a refresh really happened and it is still not
// there (a refresh held back by the half minute limit is a retry, not a no).
async function keysOf(id) {
  // A proof naming a keyset id no mint could have is the proof's own fault.
  if (typeof id !== 'string' || !/^(00[0-9a-f]{14}|01[0-9a-f]{64})$/.test(id)) throw new Refusal(`that ecash names a keyset id (${String(id).slice(0, 20)}) no mint uses`);
  await ks();
  if (!keyset.list[id]) {
    const before = keysetAt;
    await ks(true);
    if (!keyset.list[id]) {
      if (keysetAt === before) throw new Error(`keyset ${id} is not on the mint's list yet; it is looked up again shortly`);
      throw new Error(`ecash from keyset ${id}, which this mint does not list for sat`);
    }
  }
  return keysFor(id);
}
// The keysets of everything in the vault, checked before a spend.
async function vaultKeys() {
  for (const id of new Set(st.proofs.map((p) => p.id))) { try { await keysOf(id); } catch { /* left out of spends */ } }
}
// The mint's input fee: each input's own keyset rate, summed, rounded up once.
function fee(proofs) {
  let ppk = 0;
  for (const p of proofs) {
    const e = verified.get(p.id);
    if (!e) throw new Refusal(`ecash from keyset ${p.id}, whose keys this page has not checked`);
    ppk += e.ppk;
  }
  return Math.ceil(ppk / 1000);
}
const MAX_PROOFS = 64;          // per receive: more than any real token
function split(amount) {
  if (!Number.isSafeInteger(amount) || amount <= 0) throw new Refusal('that amount is not a whole number of sats');
  const out = [];
  const denoms = Object.keys(keyset.keys).map(Number).filter((d) => Number.isSafeInteger(d) && d > 0).sort((a, b) => b - a);
  for (const d of denoms) {
    const n = Math.floor(amount / d);
    if (out.length + n > MAX_PROOFS) throw new Refusal('that amount needs more pieces than this page makes at once');
    for (let i = 0; i < n; i++) out.push(d);
    amount -= n * d;
  }
  if (amount) throw new Refusal('amount cannot be split into this mint\'s denominations');
  return out;
}
// The states of these Ys, matched one by one ('UNKNOWN' when the mint did
// not answer for one exactly once).
async function stateOf(Ys) {
  const rows = (await api('/v1/checkstate', { Ys })).states;
  if (!Array.isArray(rows)) throw new Error('the mint gave a malformed answer');
  return Ys.map((y) => {
    const m = rows.filter((x) => x && x.Y === y);
    return m.length === 1 ? m[0].state : 'UNKNOWN';
  });
}

// ---- keys ----
function pubAt(xpubStr, i) {
  const xpub = xpubStr.slice(xpubStr.indexOf(']') + 1);
  return hex(HDKey.fromExtendedKey(xpub).deriveChild(i).publicKey);
}
// The change lock exactly as the signer rebuilds it (spec: no spaces).
const changeSecret = (pub, nonce) =>
  `["P2PK",{"nonce":"${nonce}","data":"${pub}","tags":[["sigflag","SIG_ALL"]]}]`;
const reserveCounter = (gen) => commit(gen, (s) => s.next++);

// A lock the signer will sign for, read by the same rules as the signer's
// own parser (main/kiss_ecash.c parse_lock_in): ["P2PK",{...}] with the
// keys nonce, data and tags each exactly once in any order, whitespace
// allowed between tokens, no escapes or control bytes inside strings, a
// nonce of 1 to 131 bytes, data this key, and tags exactly
// [["sigflag","SIG_ALL"]]; at most 1024 bytes in all. The secret's own
// bytes are kept as they are. Ecash the signer refuses is ecash nobody can
// spend, and in a spend it would block every other proof beside it.
function signerLock(pub, secret) {
  if (typeof secret !== 'string') return false;
  const b = enc(secret);
  if (b.length === 0 || b.length > 1024) return false;
  let i = 0;
  const ws = () => { while (i < b.length && (b[i] === 32 || b[i] === 9 || b[i] === 10 || b[i] === 13)) i++; };
  const eat = (c) => { ws(); if (b[i] === c) { i++; return true; } return false; };
  const str = (cap) => {   // cap: the most bytes the signer's buffer holds
    if (!eat(34)) return null;
    const s0 = i;
    while (i < b.length && b[i] !== 34) {
      if (b[i] < 0x20 || b[i] === 0x7f || b[i] === 92) return null;
      i++;
    }
    if (i >= b.length || i - s0 > cap) return null;
    const v = new TextDecoder().decode(b.slice(s0, i));
    i++;
    return v;
  };
  if (!eat(91) || str(7) !== 'P2PK' || !eat(44) || !eat(123)) return false;
  const seen = {};
  do {
    const key = str(7);
    if (key === null || seen[key] || !eat(58)) return false;
    seen[key] = true;
    if (key === 'nonce') { const n = str(131); if (!n) return false; }
    else if (key === 'data') { if (str(66) !== pub) return false; }
    else if (key === 'tags') {
      if (!eat(91) || !eat(91) || str(15) !== 'sigflag' || !eat(44) || str(15) !== 'SIG_ALL' || !eat(93) || !eat(93)) return false;
    } else return false;
  } while (eat(44));
  if (!eat(125) || !eat(93)) return false;
  ws();
  return i === b.length && seen.nonce && seen.data && seen.tags;
}

// Outputs are kept with their blinding factor as hex and the keyset they
// are for, so a record written before a swap can rebuild every proof
// after it, even once the mint has moved to a new keyset.
function blind(amount, secret) {
  const bm = blindMessage(enc(secret));
  return { amount, id: keyset.id, secret, r: bm.r.toString(16).padStart(64, '0'), B_: bm.B_.toHex(true) };
}
const wire = (o) => ({ amount: o.amount, id: o.id || keyset.id, B_: o.B_ });

// Fresh outputs locked to one new key of the signer.
function lockedOutputs(gen, amount) {
  const pieces = split(amount);
  const counter = reserveCounter(gen);
  const pub = pubAt(st.xpub, counter);
  return pieces.map((a) => ({ ...blind(a, createP2PKsecret(pub, SIGALL)), keep: counter }));
}

// The mint's signatures, checked before anything is saved: one per output,
// in order, each for the amount (and keyset) asked, each with the mint's
// DLEQ proof that it signed this output's own B_ with the checked key of
// that keyset for that amount. An answer that fails any of this saves
// nothing.
// One signature checked against the one output it is for.
async function checkOne(o, s) {
  if (!s || typeof s.C_ !== 'string' || typeof s.id !== 'string' || Number(String(s.amount)) !== o.amount ||
      (o.id && s.id !== o.id) || !s.dleq || typeof s.dleq.e !== 'string' || typeof s.dleq.s !== 'string')
    throw new Error('the mint\'s answer does not match the outputs it was given');
  const { keys } = await keysOf(s.id);
  if (!(String(o.amount) in keys)) throw new Error(`keyset ${s.id} has no key for ${o.amount}`);
  let p;
  try {
    const C = unblindSignature(pointFromHex(s.C_), BigInt('0x' + o.r), pointFromHex(keys[o.amount]));
    p = { amount: o.amount, id: s.id, secret: o.secret, C: C.toHex(true) };
  } catch { throw new Error('the mint\'s answer holds a point that is not on the curve'); }
  if (!hasValidDleq({ ...p, dleq: { e: s.dleq.e, s: s.dleq.s, r: o.r } }, { id: s.id, keys }))
    throw new Error('the mint\'s signature on an output did not check out');
  return p;
}
async function checkPromises(outs, sigs) {
  if (!Array.isArray(sigs) || sigs.length !== outs.length) throw new Error('the mint answered for a different number of outputs');
  const proofs = [];
  for (let i = 0; i < outs.length; i++) proofs.push(await checkOne(outs[i], sigs[i]));
  return proofs;
}

// ---- operations sent to the mint ----
// Before any call that makes the mint sign outputs, the whole operation is
// saved as a record: its outputs (secrets, blinding factors, keyset, which
// signer key they belong to) and what it takes to send it again (the
// quote, the inputs, the signed inputs of a spend). Each operation has one
// id for life (rid: the quote, the input secrets, or the signed hash), so
// it cannot get two records. Rule 1 above decides when a record leaves:
// only in finish(), with every output checked. A record the mint has not
// finished stays pending and is tried again: first by asking the mint what
// it already signed (NUT-09 restore), then by sending the same request.
// The owner sees it listed as unfinished and may set it aside, which stops
// the sending but keeps the record, and its outputs are still looked for.
const inFlight = new Set();
function startPending(gen, rec) {
  rec.id = rand32();
  rec.at = Date.now();
  commit(gen, (s) => {
    if (s.done.includes(tag(rec.rid))) throw new Refusal('this page already did that; nothing more to do');
    if (s.pending.some((r) => r.rid === rec.rid)) throw new Refusal('that is already listed under unfinished in the vault');
    if (s.archive.some((r) => r.rid === rec.rid)) throw new Refusal('that is set aside in the vault; tap Resume there to send it again');
    s.pending.push(rec);
    inFlight.add(rec.id);   // being sent from here on, by the caller
  });
  return rec;
}
// Whether this vault already holds, holds apart, or has spent a proof.
const known = (s) => {
  const have = new Set([...s.proofs, ...s.aside].map((p) => p.secret)), gone = new Set(s.spent);
  return (secret) => have.has(secret) || gone.has(tag(secret));
};
// Settle a record in one write: its spent inputs leave, its kept outputs
// join the vault (unless already known), a payment goes to the outbox
// (unless already there or handed over), and its id is remembered.
let lastPay = null;
function finish(gen, rec, proofs, outs = rec.outs) {
  return commit(gen, (s) => {
    const where = ['pending', 'archive'].find((k) => s[k].some((r) => r.id === rec.id));
    if (!where) return null;
    s[where] = s[where].filter((r) => r.id !== rec.id);
    const spentNow = new Set(rec.spends || []);
    s.proofs = s.proofs.filter((p) => !spentNow.has(p.secret));
    s.aside = s.aside.filter((p) => !spentNow.has(p.secret));
    s.spent = [...s.spent, ...[...spentNow].map(tag)];
    const isKnown = known(s);
    const pay = [];
    outs.forEach((o, i) => {
      const p = proofs[i];
      if (o.keep === null || o.keep === undefined) pay.push(p);
      else if (!isKnown(p.secret)) s.proofs.push({ ...p, counter: o.keep });
    });
    if (pay.length) {
      const key = listTag(pay.map((p) => p.secret));
      const token = getEncodedToken({ mint: MINT, unit: 'sat', proofs: pay });
      const seen = [...s.outbox, ...s.sent];
      if (!s.sentKeys.includes(key) && !seen.some((x) => x.key === key || x.token === token)) {
        s.outbox.push({ id: rec.id, key, amount: pay.reduce((t, p) => t + p.amount, 0), token });
        lastPay = rec.id;
      }
    }
    s.done = [...s.done, tag(rec.rid), outsTag(rec), ...(rec.prev || []).map((o) => outsTag({ outs: o }))];
    return { pay };
  });
}
// Why a record is still open, kept with it for the owner to see.
function note(gen, rec, why) {
  try {
    commit(gen, (s) => {
      const r = [...s.pending, ...s.archive].find((x) => x.id === rec.id);
      if (r) { r.note = why; r.tries = (r.tries || 0) + 1; }
    });
  } catch { /* vault gone */ }
}
// The mint's signatures on every output of a record (NUT-09 restore), null
// when it signed none of them yet. Every row must be one of the record's
// own outputs, once.
// What the mint has signed of a set of outputs (NUT-09 restore). Every row
// must be one of these outputs, once; each signature is checked on its own
// and kept with the record (got, by B_), so answers that each cover part
// of the set add up. Returns the proofs once every output is covered, null
// until then.
async function restored(gen, rec, outs = rec.outs) {
  const r = await api('/v1/restore', { outputs: outs.map(wire) });
  const rows = r.outputs, sigs = r.signatures || r.promises;
  if (!Array.isArray(rows) || !Array.isArray(sigs) || rows.length !== sigs.length || rows.length > outs.length)
    throw new Error('the mint gave a malformed restore answer');
  const byB = new Map(outs.map((o) => [o.B_, o])), seen = new Set();
  for (const row of rows) {
    const B = row && row.B_;
    if (!byB.has(B) || seen.has(B)) throw new Error('the mint\'s restore answer names outputs it was not asked about');
    seen.add(B);
  }
  // Each signature on its own: a bad one is not kept, the good ones are.
  const got = { ...(rec.got || {}) };
  let added = 0, bad = null;
  for (let i = 0; i < rows.length; i++) {
    const B = rows[i].B_;
    if (got[B]) continue;
    try { got[B] = await checkOne(byB.get(B), sigs[i]); added++; } catch (e) { bad = bad || e; }
  }
  if (added) {
    rec.got = got;
    try {
      commit(gen, (s) => {
        const x = [...s.pending, ...s.archive].find((y) => y.id === rec.id);
        if (x) x.got = { ...(x.got || {}), ...got };
      });
    } catch { /* kept in this tab; asked again next time */ }
  }
  if (outs.every((o) => got[o.B_])) return outs.map((o) => got[o.B_]);
  if (bad) throw bad;
  return null;
}
async function send(rec) {
  const outputs = rec.outs.map(wire);
  if (rec.kind === 'mint') {
    if (!rec.quote) throw new Error('an old test mint record with no quote: it can only be restored');
    return (await api('/v1/mint/bolt11', { quote: rec.quote, outputs })).signatures;
  }
  if (!rec.inputs) throw new Error('an old spend record without its signed inputs: it can only be restored');
  return (await api('/v1/swap', { inputs: rec.inputs, outputs })).signatures;
}
// Carry one record as far as the mint lets it. A fresh one is sent first;
// one being recovered asks what the mint already signed before sending
// again, the same request each time, and only while it is still pending in
// the same vault. A set aside record is only ever restored, never sent.
async function drive(gen, rec, mode) {
  if (inFlight.has(rec.id) && mode !== 'fresh') throw new Error('already being sent');
  inFlight.add(rec.id);
  try {
    await ks();
    if (mode !== 'fresh') {
      // Output sets made before a keyset change are still looked for. A
      // lookup or a check that fails here does not hold up the others.
      for (const prev of rec.prev || []) {
        let old = null;
        try { old = await restored(gen, rec, prev); } catch { continue; }
        if (old) return finish(gen, rec, old, prev);
      }
    }
    let proofs = mode === 'fresh' ? null : await restored(gen, rec);
    if (!proofs && mode === 'aside') return null;
    if (!proofs) {
      const cur = load();
      if (!cur || cur.gen !== gen || !cur.pending.some((r) => r.id === rec.id)) throw new Refusal('the vault changed, so this was not sent');
      try { proofs = await checkPromises(rec.outs, await send(rec)); } catch (e) {
        if (e.code === undefined) throw e;
        proofs = await restored(gen, rec);
        if (!proofs && [11002, 12001, 12002].includes(e.code)) await reoutput(gen, rec, e);
        if (!proofs) throw e;
      }
    }
    return finish(gen, rec, proofs);
  } catch (e) {
    if (mode !== 'aside') note(gen, rec, e.message || String(e));
    throw e;
  } finally {
    inFlight.delete(rec.id);
    try { render(); } catch { /* shown next time */ }
  }
}
// The mint no longer takes the keyset a record's outputs were made for. A
// test mint quote or a pasted token swap gets new outputs for the same
// amount under the current keyset (the old ones stay in prev, still looked
// for); the next try sends those. A spend cannot: its outputs are signed by
// the signer, so it is left for the owner to set aside and make again.
async function reoutput(gen, rec, e) {
  if (rec.kind === 'spend') { e.message += '; this spend\'s outputs no longer fit what the mint takes: set it aside and make the spend again'; return; }
  await ks(true);
  // The amount the outputs must carry, with fees as the mint lists them now.
  let total = rec.outs.reduce((t, o) => t + o.amount, 0);
  if (rec.kind === 'bearer') {
    for (const id of new Set(rec.inputs.map((p) => p.id))) await keysOf(id);
    total = rec.inputs.reduce((t, p) => t + p.amount, 0) - fee(rec.inputs);
    if (total <= 0) { e.message += '; at the mint\'s current fee that ecash is worth nothing to swap'; return; }
  }
  if (rec.outs.every((o) => o.id === keyset.id) && rec.outs.reduce((t, o) => t + o.amount, 0) === total) return;
  const fresh = lockedOutputs(gen, total);
  commit(gen, (s) => {
    const r = s.pending.find((x) => x.id === rec.id);
    if (!r) return;
    r.prev = [...(r.prev || []), r.outs];
    r.outs = fresh;
  });
  rec.prev = [...(rec.prev || []), rec.outs];
  rec.outs = fresh;
  e.message += '; the mint\'s keyset or fee changed, so new outputs were made and are sent on the next try';
}
const why = (e) => String((e && e.message) || e).replace(/\.+$/, '');
const KEPT = 'It is listed under unfinished in the vault, with everything needed to finish it; nothing was lost.';
async function recoverPending(gen) {
  let done = 0, open = 0, first = null;
  for (const rec of [...st.pending]) {
    if (!st || st.gen !== gen) return;
    if (inFlight.has(rec.id)) continue;   // being sent right now
    try {
      if (await drive(gen, rec, 'again')) done++;
    } catch (e) { open++; first = first || why(e); }
  }
  // One message for the whole pass, so one swap's news does not hide another's.
  if (done || open) {
    const parts = [];
    if (done) parts.push(`Finished ${done === 1 ? 'a swap that was' : `${done} swaps that were`} interrupted.`);
    if (open) parts.push(`${open === 1 ? 'One unfinished swap is' : `${open} unfinished swaps are`} still not done (${first}). ${open === 1 ? 'It is' : 'They are'} listed under unfinished in the vault, with everything needed to finish ${open === 1 ? 'it' : 'them'}; nothing was lost.`);
    msg('recv-msg', parts.join(' '), open ? 'bad' : 'ok');
  }
  for (const rec of [...st.archive]) {
    if (!st || st.gen !== gen) return;
    if (inFlight.has(rec.id)) continue;
    try { if (await drive(gen, rec, 'aside')) msg('recv-msg', 'A swap that was set aside turned out finished; its ecash is back in the vault.', 'ok'); } catch { /* still set aside */ }
  }
  await checkBalance(gen);
}
// On every load and every Try again, the vault's proofs and the ones held
// apart are checked with the mint, each by its own secret (a snapshot
// taken before asking, applied by secret inside the write). A proof the
// mint calls spent moves apart, kept and never deleted, unless an
// unfinished spend is using it: an older tab, or a swap the page set
// aside, may have spent it. One held apart that the mint now calls unspent
// comes back, unless this vault itself spent it.
let balanceRun = Promise.resolve();
function checkBalance(gen) {
  balanceRun = balanceRun.then(() => checkBalanceNow(gen), () => checkBalanceNow(gen));
  return balanceRun;
}
async function checkBalanceNow(gen) {
  if (!st || st.gen !== gen) return;
  const busy = new Set(st.pending.flatMap((r) => r.spends || []));
  const ask = [...st.proofs.filter((p) => !busy.has(p.secret)), ...st.aside].map((p) => p.secret);
  if (!ask.length) return;
  // Each batch the mint answers counts, even if a later one fails.
  const state = new Map();
  for (let i = 0; i < ask.length; i += 100) {
    const part = ask.slice(i, i + 100);
    let res;
    try { res = await stateOf(part.map(yOf)); } catch { break; /* the rest is checked next time */ }
    part.forEach((x, j) => state.set(x, res[j]));
  }
  // A proof the mint calls pending may be on its way out: it is not offered
  // for a spend, nor counted, until the mint says unspent again.
  for (const [x, v] of state) { if (v === 'PENDING') mintPending.add(x); else if (v === 'UNSPENT' || v === 'SPENT') mintPending.delete(x); }
  const moved = (s) => {
    const gone = new Set(s.spent), held = new Set(s.pending.flatMap((r) => r.spends || []));
    const have = new Set(s.proofs.map((p) => p.secret));
    const back = [];
    for (const p of s.aside) {
      if (state.get(p.secret) !== 'UNSPENT' || gone.has(tag(p.secret)) || have.has(p.secret)) continue;
      have.add(p.secret);   // each secret comes back once
      back.push(p);
    }
    return { out: s.proofs.filter((p) => state.get(p.secret) === 'SPENT' && !held.has(p.secret)), back };
  };
  const first = moved(st);
  if (!first.out.length && !first.back.length) { try { render(); } catch { /* shown next time */ } return; }
  commit(gen, (s) => {
    const { out, back } = moved(s);
    const o = new Set(out.map((p) => p.secret)), b = new Set(back.map((p) => p.secret));
    s.proofs = [...s.proofs.filter((p) => !o.has(p.secret)), ...back];
    s.aside = [...s.aside.filter((p) => !b.has(p.secret)), ...out];
  });
}
// The owner's call: send the oldest set aside record again, the same
// record and outputs, exactly as before it was set aside.
async function resume() {
  const gen = st.gen;
  const rec = st.archive[0];
  if (!rec || inFlight.has(rec.id)) return;
  commit(gen, (s) => {
    const r = s.archive.find((x) => x.id === rec.id);
    if (!r) return;
    s.archive = s.archive.filter((x) => x.id !== rec.id);
    s.pending.push(r);
  });
  msg('recv-msg', 'Sending the set aside swap again...');
  if (await drive(gen, rec, 'again')) msg('recv-msg', 'Finished the swap that was set aside.', 'ok');
}
async function runFresh(gen, rec) {
  try { return await drive(gen, rec, 'fresh'); } catch (e) {
    throw new Error(`Not finished: ${why(e)}. ${KEPT}`);
  }
}
// The owner's call on the oldest unfinished record: set it aside. Inputs
// of a set aside spend that the mint says are spent move out of the
// balance into the aside list, kept, never deleted.
async function setAside() {
  const gen = st.gen;
  const rec = st.pending.find((r) => !inFlight.has(r.id));
  if (!rec) return;
  if (!confirm(`Set this unfinished swap aside?\n\n${rec.note || 'It has not finished yet.'}\n\nThe page stops sending it but keeps its record, and still picks it up if the mint shows it finished.`)) return;
  let spentIn = [];
  if (rec.kind === 'spend') {
    try {
      const states = await stateOf(rec.spends.map(yOf));
      spentIn = rec.spends.filter((x, i) => states[i] === 'SPENT');
    } catch { /* leave the inputs in the balance */ }
  }
  commit(gen, (s) => {
    const r = s.pending.find((x) => x.id === rec.id);
    if (!r) return;
    s.pending = s.pending.filter((x) => x.id !== rec.id);
    s.archive.push(r);
    s.aside.push(...s.proofs.filter((p) => spentIn.includes(p.secret)));
    s.proofs = s.proofs.filter((p) => !spentIn.includes(p.secret));
  });
}

// ---- receive ----
// The quote and its outputs are saved before waiting for payment, so
// closing the page while it waits loses nothing.
async function mintTest() {
  const amount = Number($('mint-amt').value);
  const gen = st.gen;
  await ks();
  if (!st || st.gen !== gen) throw new Refusal('the vault changed; nothing was asked of the mint');
  if (!Number.isSafeInteger(amount) || amount <= 0 || amount > 100000) throw new Refusal('ask for 1 to 100000 test sats');
  msg('recv-msg', 'Asking the test mint...');
  const q = await api('/v1/mint/quote/bolt11', { amount, unit: 'sat' });
  if (typeof q.quote !== 'string' || !q.quote) throw new Error('the test mint gave no quote');
  const outs = lockedOutputs(gen, amount);
  const rec = startPending(gen, { kind: 'mint', rid: `q:${q.quote}`, quote: q.quote, outs });
  // Waiting for payment; any failure here is left to the mint request
  // below, which records it on the record.
  try {
    const until = Date.now() + 20000;
    for (let i = 0; i < 30 && Date.now() < until; i++) {
      const s = await api(`/v1/mint/quote/bolt11/${encodeURIComponent(q.quote)}`, null, until - Date.now());
      if (s.state === 'PAID' || s.state === 'ISSUED') break;
      await new Promise((r) => setTimeout(r, 500));
    }
  } catch { /* below */ }
  await runFresh(gen, rec);
  msg('recv-msg', `${sats(amount)} locked to signer key ${outs[0].keep}.`, 'ok');
}

// Proofs as they arrive, from a token or a Nostr payload, made plain and
// bounded before anything is computed from them.
function cleanProofs(list) {
  if (!Array.isArray(list) || list.length === 0) throw new Refusal('there is no ecash in that');
  if (list.length > MAX_PROOFS) throw new Refusal(`that holds ${list.length} proofs; this page takes up to ${MAX_PROOFS} at once`);
  const out = list.map((p) => {
    const amount = Number(String(p && p.amount));
    if (!Number.isSafeInteger(amount) || amount <= 0 || amount > 2 ** 40) throw new Refusal('that ecash has an amount no mint issues');
    if (typeof p.secret !== 'string' || typeof p.C !== 'string' || typeof p.id !== 'string') throw new Refusal('that ecash is malformed');
    return { amount, id: p.id, secret: p.secret, C: p.C, dleq: p.dleq };
  });
  if (new Set(out.map((p) => p.secret)).size !== out.length) throw new Refusal('that ecash repeats a proof');
  if (new Set(out.map((p) => p.id)).size > 4) throw new Refusal('that ecash comes from too many keysets');
  return out;
}
// Each proof's keyset must be one of the mint's sat keysets, checked, not
// past its final expiry, with a key for the proof's amount.
async function checkDenoms(proofs) {
  for (const p of proofs) {
    const e = await keysOf(p.id);
    if (expired(e)) throw new Error('that ecash is from a keyset the mint says it no longer redeems');
    if (!(String(p.amount) in e.keys)) throw new Refusal('that ecash has an amount its keyset does not issue');
  }
}

// Unlocked ecash is bearer cash, so it is swapped at once into ecash locked
// to the signer. Pasted only: one sent over Nostr is refused (see onWrap).
async function swapBearer(gen, proofs) {
  await checkDenoms(proofs);
  // A damaged signature is refused before anything is saved: each C must be
  // a curve point, and a DLEQ proof, when the token carries one, must hold.
  for (const p of proofs) {
    try { p.C = pointFromHex(p.C).toHex(true); } catch { throw new Refusal('that token holds a signature that is not a curve point; ask the sender for it again'); }
    if (p.dleq && !hasValidDleq(p, { id: p.id, keys: (await keysOf(p.id)).keys }))
      throw new Refusal('that token\'s proof of the mint\'s signature does not check out; ask the sender for it again');
  }
  const plain = proofs.map(({ dleq, ...p }) => p);
  const states = await stateOf(plain.map((p) => yOf(p.secret)));
  if (states.includes('SPENT')) throw new Refusal('part of that token was already spent, so the mint will not take it');
  if (!states.every((x) => x === 'UNSPENT')) throw new Error('the mint has not said that token is unspent yet; try again in a moment');
  const total = plain.reduce((s, p) => s + p.amount, 0);
  const keep = total - fee(plain);
  if (keep <= 0) throw new Refusal('that ecash is smaller than the mint fee');
  const outs = lockedOutputs(gen, keep);
  await runFresh(gen, startPending(gen, { kind: 'bearer', rid: 'in:' + listTag(plain.map((p) => JSON.stringify([p.amount, p.id, p.secret, p.C]))), inputs: plain, outs }));
  return `${sats(keep)} arrived unlocked and is now locked to signer key ${outs[0].keep} (mint fee ${sats(total - keep)}).`;
}

async function receiveToken() {
  const gen = st.gen;
  // The text is taken once, before anything waits: the box may be edited
  // meanwhile, and it is cleared only if it still holds this text.
  const text = $('token-in').value.trim();
  if (!text) throw new Refusal('paste a token first');
  await ks();
  // A token from a keyset added since the page loaded names it by a short
  // id only the mint's current list resolves.
  let tok;
  try { tok = getDecodedToken(text, keyset.ids); } catch {
    await ks(true);
    try { tok = getDecodedToken(text, keyset.ids); } catch (e) {
      if (/keyset/i.test(e.message || '')) throw new Error('that token names a keyset this mint does not list; if the mint just added it, try again in a minute');
      throw new Refusal('that is not a cashu token this page can read');
    }
  }
  if (tok.mint.replace(/\/$/, '') !== MINT) throw new Refusal(`that token is from ${tok.mint}; this page uses ${MINT}`);
  const proofs = cleanProofs(tok.proofs);
  const locked = proofs.filter((p) => p.secret.startsWith('['));
  let said;
  if (locked.length) {
    if (!st.jar) throw new Refusal('that ecash is locked to something other than your tip jar, so this page cannot take it');
    said = await keepLockedTip(gen, st.jar.counter, proofs, (t) => msg('recv-msg', t, 'ok')) || 'that tip is already in the vault.';
  } else said = await swapBearer(gen, proofs);
  if ($('token-in').value.trim() === text) $('token-in').value = '';
  msg('recv-msg', said, 'ok');
}

// ---- tip jar ----
// One printable NUT-18 payment request whose nut10 field asks the payer's
// wallet to lock the tip to one key of the signer, SIG_ALL, nothing else.
// Every tip shares that key, which is what lets one spend take many of them
// (NUT-11 wants every SIG_ALL input on the same key); the cost is that the
// mint can link the tips to each other.
//
// Delivery is Nostr (NIP-17), the one route wallets such as cashu.me pay a
// request by: the tip comes to a Nostr key this page makes and keeps. Only
// tips locked to the signer are taken that way, so a copy of that key, or
// of this page's storage, can see tips arrive and spend none of the ones
// kept. (A payer who sends an unlocked tip anyway sends bearer cash to that
// key; the page leaves it, and a copy of the key could take it.)
const RELAYS = ['wss://relay.primal.net', 'wss://relay.damus.io', 'wss://relay.snort.social'];
function jarKeys() {
  if (!st.jar || !st.jar.nsk) {
    const gen = st.gen;
    const counter = st.jar ? st.jar.counter : reserveCounter(gen);
    const nsk = hex(generateSecretKey());
    commit(gen, (s) => { if (!s.jar || !s.jar.nsk) s.jar = { counter, nsk, seen: [] }; });
  }
  const sk = Uint8Array.from(st.jar.nsk.match(/../g), (b) => parseInt(b, 16));
  return { sk, pk: getPublicKey(sk) };
}
function jarRequest() {
  const { pk } = jarKeys();
  const nostr = { type: 'nostr', target: nprofileEncode({ pubkey: pk, relays: RELAYS }), tags: [['n', '17']] };
  const pr = new PaymentRequest([nostr], `kiss${st.fp}`, undefined, 'sat', [MINT],
    'tips to a KISS Signer vault', false,
    { kind: 'P2PK', data: pubAt(st.xpub, st.jar.counter), tags: SIGALL });
  return pr.toEncodedRequest();
}
function showJar() {
  const creq = jarRequest();
  const t = $('jar-text');
  t.value = creq;
  drawQR($('jar-qr'), creq);
  drawQR($('jar-qr-inline'), creq);
  $('jar-box').classList.remove('hidden');
  t.style.height = 'auto';
  t.style.height = t.scrollHeight + 2 + 'px';
  listenJar();
}

// Gift wraps addressed to the jar key, all of them: NIP-17 backdates a wrap
// by up to two days, so a time window would drop tips that arrived while the
// page was closed. A wrap is marked handled only once nothing in it is
// left to try; one that met a mint or network failure is tried again a
// little later, and the next time the page opens. The listener belongs to
// one pairing and one jar key: forget the signer and it closes, and work it
// started is refused if either changed meanwhile.
let jarPool = null, jarQueue = Promise.resolve();
function listenJar() {
  if (jarPool || !owner || !st || !st.jar || !st.jar.nsk) return;
  const { sk, pk } = jarKeys();
  const w = { gen: st.gen, counter: st.jar.counter, sk };
  jarPool = new SimplePool();
  jarPool.subscribe(RELAYS, { kinds: [1059], '#p': [pk] }, {
    onevent: (ev) => { jarQueue = jarQueue.then(() => onWrap(w, ev, 0)); },
  });
  msg('jar-msg', 'Listening for tips over Nostr while this page is open.');
}
function stopJar() {
  if (jarPool) { try { jarPool.close(RELAYS); } catch { /* already closed */ } }
  jarPool = null;
}
const markSeen = (w, id) => commit(w.gen, (s) => {
  if (!s.jar || s.jar.counter !== w.counter) return;
  s.jar.seen = [...(s.jar.seen || []).filter((x) => x !== id), id].slice(-2000);
});
const live = (w) => st && st.gen === w.gen && st.jar && st.jar.counter === w.counter;
async function onWrap(w, ev, attempt) {
  if (!live(w) || (st.jar.seen || []).includes(ev.id)) return;
  let payload;
  try { payload = JSON.parse(unwrapEvent(ev, w.sk).content); } catch { return; }   // not for us, or not a tip
  const say = (t) => msg('jar-msg', `Tip over Nostr: ${t}`, 'ok');
  try {
    await ks();
    if (!payload || typeof payload !== 'object') throw new Refusal('a Nostr message that is not a tip');
    if (String(payload.mint || '').replace(/\/$/, '') !== MINT) throw new Refusal(`a tip from another mint (${payload.mint})`);
    const proofs = cleanProofs(payload.proofs);
    const pub = pubAt(st.xpub, w.counter);
    if (!proofs.some((p) => signerLock(pub, p.secret)))
      throw new Refusal('a tip came over Nostr without the lock the tip jar asks for, and was left; ask the sender to pay the tip jar again');
    const said = await keepLockedTip(w.gen, w.counter, proofs, say);
    markSeen(w, ev.id);
    if (said) say(said);
  } catch (e) {
    if (!live(w)) return;
    if (e instanceof Refusal) {
      try { markSeen(w, ev.id); } catch { /* vault gone */ }
      msg('jar-msg', `A tip arrived over Nostr but was not kept: ${e.message || e}`, 'bad');
    } else {
      msg('jar-msg', `A tip arrived over Nostr and is waiting for the mint (${e.message || e}); it is tried again.`, 'bad');
      if (attempt < 10) setTimeout(() => { jarQueue = jarQueue.then(() => onWrap(w, ev, attempt + 1)); }, 30000);
    }
  }
}

// A tip that came locked is kept as it is, with no swap: the payer cannot
// take it back, since only the signer can spend it. Checked first, because
// the page cannot spend it to find out later: the lock (above), the keyset
// (checked keys, not expired, a key for the amount), the mint's own proof
// that it signed each one (DLEQ), and the mint's word, matched by Y, that
// each one is unspent. Proofs already known to the vault are skipped, so a
// tip delivered twice is kept once. Proofs the mint calls spent are kept
// apart (see below); the rest join the vault. If the mint has not answered
// for some, those are tried again (an error after the others are kept).
// Returns what was kept, or null when nothing in it was new.
async function keepLockedTip(gen, counter, proofs, progress) {
  // Each proof is judged on its own: a bad one is left out, and the good
  // ones beside it are still kept.
  const pub = pubAt(st.xpub, counter);
  const isKnown = known(st);
  const kept = [], left = [];
  for (const p of proofs) {
    if (!signerLock(pub, p.secret)) { left.push('a proof locked to something other than your tip jar'); continue; }
    if (isKnown(p.secret)) continue;
    let e;
    try { e = await keysOf(p.id); } catch (x) {   // a mint side problem is retried, never final
      if (!(x instanceof Refusal)) throw x;
      left.push('a proof naming a keyset this mint does not use'); continue;
    }
    if (expired(e) || !(String(p.amount) in e.keys)) { left.push('a proof with an amount or keyset the mint does not redeem'); continue; }
    let C;
    try { C = pointFromHex(p.C).toHex(true); } catch { left.push('a proof whose signature is not a curve point'); continue; }
    if (!p.dleq || !hasValidDleq(p, { id: p.id, keys: e.keys })) { left.push('a proof without the mint\'s proof that it signed it'); continue; }
    kept.push({ amount: p.amount, id: p.id, secret: p.secret, C });
  }
  const leftSaid = left.length ? ` Left out: ${[...new Set(left)].join('; ')}.` : '';
  if (!kept.length) {
    if (left.length) throw new Refusal(`nothing in that could be kept.${leftSaid}`);
    return null;
  }
  // Proofs the mint calls unspent join the vault. Ones it calls spent are
  // genuine (their DLEQ held) and locked to the signer, so only the signer
  // could have spent them: they are held apart, kept, and come back if the
  // mint later calls them unspent. Any other answer is asked again.
  const states = await stateOf(kept.map((p) => yOf(p.secret)));
  const good = kept.filter((p, i) => states[i] === 'UNSPENT');
  const held = kept.filter((p, i) => states[i] === 'SPENT');
  const waiting = states.filter((x) => x !== 'UNSPENT' && x !== 'SPENT').length;
  const [added, apart] = good.length || held.length ? commit(gen, (s) => {
    if (!s.jar || s.jar.counter !== counter) throw new Refusal('the tip jar changed while this tip was checked');
    const k = known(s);
    const add = good.filter((p) => !k(p.secret)), hold = held.filter((p) => !k(p.secret));
    const fix = new Set([...add, ...hold].map((p) => p.secret));
    s.proofs = [...s.proofs.filter((p) => !fix.has(p.secret)), ...add.map((p) => ({ ...p, counter }))];
    s.aside = [...s.aside, ...hold.map((p) => ({ ...p, counter }))];
    return [add, hold];
  }) : [[], []];
  const total = added.reduce((t, p) => t + p.amount, 0), apartSum = apart.reduce((t, p) => t + p.amount, 0);
  let said = total ? `${sats(total)} arrived locked to your signer; kept as it is, no fee.${leftSaid}` : leftSaid.trim();
  if (apartSum) said += `${said ? ' ' : ''}${sats(apartSum)} of it the mint calls already spent; it is kept apart and comes back if the mint says otherwise.`;
  if (waiting) {
    if (said) progress(said);
    throw new Error('the mint has not confirmed part of the tip yet');
  }
  return said || null;
}

// ---- send ----
let openReq = null;
const MAX_IN = 32, MAX_OUT = 48;   // the signer's limits (kiss_ecash.h)

// NUT-11 SIG_ALL: the hash the signer's reply must carry.
function sigallHash(inputs, outs) {
  let m = '';
  for (const p of inputs) m += p.secret + p.C;
  for (const o of outs) m += String(o.amount) + o.B_;
  return hex(sha256(enc(m)));
}

// SIG_ALL spends proofs that share one key. For each key, a few ways to
// cover the payment and its fee: largest first (fewest inputs), smallest
// first (least change), the one smallest proof that covers it alone, and
// largest first among proofs no bigger than each amount the key holds
// (which leaves a huge proof out when smaller ones suffice). makeRequest
// takes the first that fits the signer's limits. Proofs the signer would
// refuse, whose keyset's keys are not checked yet or expired, that the mint
// last called pending, or that a pending spend
// already uses, are left out.
// Ecash a spend may use, and the headline counts: not refused by the
// signer, its keyset not expired (when its keys have been read), and not
// called pending by the mint at the last check.
const mintPending = new Set();
const spendable = (p) => !p.bad && !mintPending.has(p.secret) && !(verified.has(p.id) && expired(verified.get(p.id)));
function chooseInputs(amount) {
  const busy = new Set(st.pending.flatMap((r) => r.spends || []));
  const usable = (p) => spendable(p) && !busy.has(p.secret) && verified.has(p.id);
  const tries = [], seen = new Set();
  const add = (counter, take) => {
    const total = take.reduce((s, p) => s + p.amount, 0);
    const key = take.map((p) => p.secret).sort().join('\n');
    if (!take.length || take.length > MAX_IN || total < amount + fee(take) || seen.has(key)) return;
    seen.add(key);
    tries.push({ counter, proofs: take, total });
  };
  const greedy = (list) => {
    const take = [];
    let total = 0;
    for (const p of list) {
      if (take.length >= MAX_IN || total >= amount + fee(take)) break;
      take.push(p);
      total += p.amount;
    }
    return take;
  };
  for (const [counter, all] of groupBy().entries()) {
    const ok = all.filter(usable);
    const desc = [...ok].sort((a, b) => b.amount - a.amount || (a.id < b.id ? -1 : a.id > b.id ? 1 : 0)), asc = [...desc].reverse();
    add(counter, greedy(desc));
    add(counter, greedy(asc));
    const one = asc.find((p) => p.amount >= amount + fee([p]));
    if (one) add(counter, [one]);
    for (const cap of new Set(ok.map((p) => p.amount))) add(counter, greedy(desc.filter((p) => p.amount <= cap)));
    for (const take of search(amount, desc)) add(counter, take);
  }
  return tries.sort((a, b) => a.proofs.length - b.proofs.length || a.total - b.total);
}
// A bounded search over one key's proofs (largest first, at most 20,000
// steps) for the sets that cover the payment with the fewest inputs plus
// change pieces, which is what decides the request's size.
function search(amount, desc) {
  const denoms = Object.keys(keyset.keys).map(Number).filter(Number.isSafeInteger).sort((a, b) => b - a);
  const pieces = (n) => { let c = 0; for (const d of denoms) { c += Math.floor(n / d); n %= d; } return c; };
  const suffix = [];
  for (let i = desc.length - 1, t = 0; i >= 0; i--) suffix[i] = t += desc[i].amount;
  const best = [], take = [];
  let steps = 0;
  const visit = (i, total) => {
    if (++steps > 20000) return;
    const need = amount + fee(take);
    if (total >= need) {
      best.push({ score: take.length + pieces(total - need), take: [...take] });
      best.sort((a, b) => a.score - b.score);
      if (best.length > 5) best.pop();
      return;
    }
    if (i >= desc.length || take.length >= MAX_IN || total + suffix[i] < need) return;
    take.push(desc[i]);
    visit(i + 1, total + desc[i].amount);
    take.pop();
    let j = i + 1;
    while (j < desc.length && desc[j].amount === desc[i].amount && desc[j].id === desc[i].id) j++;   // leaving one out leaves out all of that amount and keyset here
    visit(j, total);
  };
  visit(0, 0);
  return best.map((b) => b.take);
}

async function makeRequest() {
  const gen = st.gen;
  await ks();
  await vaultKeys();
  const amount = Number($('send-amt').value);
  if (!Number.isSafeInteger(amount) || amount <= 0) throw new Refusal('enter an amount');
  const tries = chooseInputs(amount);
  if (!tries.length) throw new Refusal('no single signer key holds that much yet; send a smaller amount');
  let made = null;
  for (const pick of tries) {
    const change = pick.total - amount - fee(pick.proofs);
    let pay, back;
    try { pay = split(amount); back = change > 0 ? split(change) : []; } catch { continue; }
    if (pay.length + back.length > MAX_OUT) continue;
    const outs = pay.map((a) => ({ ...blind(a, rand32()), keep: null }));
    let cc = null;
    for (const a of back) {
      if (cc === null) cc = reserveCounter(gen);
      const nonce = rand32();
      outs.push({ ...blind(a, changeSecret(pubAt(st.xpub, cc), nonce)), keep: cc, nonce });
    }
    const req = JSON.stringify({ v: 1, mint: MINT, unit: 'sat', counter: pick.counter,
      inputs: pick.proofs.map((p) => ({ secret: p.secret, C: p.C })),
      outputs: outs.map((o) => (o.keep === null ? { amount: o.amount, B_: o.B_ }
        : { amount: o.amount, B_: o.B_, change: { counter: o.keep, nonce: o.nonce, r: o.r } })) });
    if (enc(req).length > REQ_MAX) continue;
    made = { pick, outs, req, change };
    break;
  }
  if (!made) throw new Refusal(`that spend needs a request bigger than the ${REQ_MAX} bytes the signer reads; send a smaller amount`);
  const { pick, outs, req, change } = made;
  openReq = { gen, counter: pick.counter, inputs: pick.proofs, outs, hash: sigallHash(pick.proofs, outs) };
  $('req-text').value = req;
  showFrames(req);
  $('req-box').classList.remove('hidden');
  msg('send-msg', `${sats(amount)} to send, ${sats(change)} back to the vault, mint fee ${sats(fee(pick.proofs))}. Your signer will show the same.`);
}

async function submitReply(text) {
  if (!openReq) throw new Refusal('no spend request open');
  let rep;
  try { rep = JSON.parse(text.trim()); } catch { throw new Refusal('that is not the signer\'s answer'); }
  holdNext = false;
  if (!rep || rep.v !== 1 || rep.hash !== openReq.hash || !/^[0-9a-f]{128}$/.test(rep.sig || ''))
    throw new Refusal('that answer is for a different request');
  // The signature itself is checked here, against the signer's key for these
  // inputs and the hash this page computed, before anything is saved: a
  // mistyped answer leaves the request open for the right one.
  let good = false;
  try { good = schnorr.verify(unhex(rep.sig), unhex(openReq.hash), unhex(pubAt(st.xpub, openReq.counter).slice(2))); } catch { /* below */ }
  if (!good) throw new Refusal('that answer\'s signature does not check out; scan or paste the signer\'s answer again');
  const { gen, outs, hash } = openReq;
  const inputs = openReq.inputs.map((p, i) => {
    const q = { amount: p.amount, id: p.id, secret: p.secret, C: p.C };
    if (i === 0) q.witness = JSON.stringify({ signatures: [rep.sig] });
    return q;
  });
  const rec = startPending(gen, { kind: 'spend', rid: `sig:${hash}`, inputs, outs, spends: inputs.map((p) => p.secret) });
  openReq = null;
  stopFrames();
  $('req-box').classList.add('hidden');
  $('reply-in').value = '';
  msg('send-msg', 'Sending to the mint...');
  await runFresh(gen, rec);
  msg('send-msg', '');
}

// ---- payments to hand over ----
// A payment stays in the outbox, across reloads, until the owner says it
// was sent. The token on screen and its QR are always the same payment:
// the QR is cleared before a new one is drawn, and a token too long for
// one QR shows as text only. The payment on screen stays there until it is
// marked sent: one that finishes meanwhile only adds to the waiting count.
// "I've sent it" acts on the payment it was shown with, and the next
// waiting payment is shown only on request, so a double tap cannot pass
// over one. Payments handed over can be looked at again.
// After "I've sent it", no payment opens by itself until the owner asks
// for the next one or starts a new spend, so a second tap never lands on a
// payment that just appeared.
let shownId = null, shownToken = null, holdNext = false;
function renderOutbox() {
  const ob = (st && st.outbox) || [];
  if (lastPay && shownId === null && !holdNext && ob.some((x) => x.id === lastPay)) shownId = lastPay;
  lastPay = null;
  if (shownId !== null && !ob.some((x) => x.id === shownId)) shownId = null;
  const cur = ob.find((x) => x.id === shownId);
  if (cur) {
    if (shownToken !== cur.token) {
      shownToken = cur.token;
      const out = $('token-out');
      out.value = cur.token;
      out.style.height = 'auto';
      out.style.height = out.scrollHeight + 2 + 'px';
      const fits = drawQR($('token-qr'), cur.token);
      $('token-qr').classList.toggle('hidden', !fits);
      $('token-qr-note').classList.toggle('hidden', fits);
    }
    $('token-done').dataset.id = cur.id;
    $('token-box').classList.remove('hidden');
  } else {
    shownToken = null;
    $('token-done').dataset.id = '';
    $('token-box').classList.add('hidden');
  }
  const waiting = ob.filter((x) => x.id !== shownId).length;
  $('next-text').textContent = waiting ? `${waiting} payment${waiting > 1 ? 's' : ''} waiting to be handed over.` : '';
  $('next-box').classList.toggle('hidden', !waiting);
  $('token-next').classList.toggle('hidden', !!cur);
  const sent = (st && st.sent) || [];
  $('sent-box').classList.toggle('hidden', !sent.length);
  $('sent-count').textContent = sent.length;
  $('sent-text').value = [...sent].reverse()
    .map((x) => `${x.amount ? sats(x.amount) : 'payment'}${x.at ? `, marked sent ${new Date(x.at).toLocaleString()}` : ''}\n${x.token}`).join('\n\n');
}
function sentIt() {
  const id = $('token-done').dataset.id;
  if (!id || id !== shownId) return;
  shownId = null;
  holdNext = true;
  commit(st.gen, (s) => {
    const x = s.outbox.find((y) => y.id === id);
    if (!x) return;
    s.outbox = s.outbox.filter((y) => y.id !== id);
    s.sent = [...s.sent, { ...x, at: Date.now() }].slice(-KEEP_SENT);
    if (!s.sentKeys.includes(x.key)) s.sentKeys.push(x.key);
  });
}
function showNext() {
  holdNext = false;
  const ob = st.outbox;
  if (ob.length) shownId = ob[0].id;
  renderOutbox();
}

// ---- QR out ----
let frameTimer = null;
// Draws text as one QR code, or clears the canvas and returns false when
// the text is too long for one: an older code is never left on screen.
function drawQR(canvas, text) {
  canvas.width = canvas.height = 0;
  let q;
  try {
    q = qrcode(0, 'L');
    q.addData(text, 'Byte');
    q.make();
  } catch { return false; }
  const n = q.getModuleCount(), pad = 4, px = 6;
  canvas.width = canvas.height = (n + 2 * pad) * px;
  const g = canvas.getContext('2d');
  g.fillStyle = '#fff'; g.fillRect(0, 0, canvas.width, canvas.height);
  g.fillStyle = '#000';
  for (let y = 0; y < n; y++) for (let x = 0; x < n; x++)
    if (q.isDark(y, x)) g.fillRect((x + pad) * px, (y + pad) * px, px, px);
  return true;
}
// "pMofN <base64>" frames: the animated format the signer already reads. A
// short request is still sent as "p1of1": the signer reads bare base64 only
// when it is a PSBT, so anything else must come framed.
function frames(text) {
  const b64 = btoa(String.fromCharCode(...enc(text)));
  const parts = [];
  for (let i = 0; i < b64.length; i += FRAME_CHARS) parts.push(b64.slice(i, i + FRAME_CHARS));
  return parts.map((p, k) => `p${k + 1}of${parts.length} ${p}`);
}
// What the signer answers with its ecash key; it must match the signer's
// KEY_REQUEST byte for byte.
const KEY_REQUEST = '{"v":1,"want":"key"}';
function showFrames(text) {
  stopFrames();
  const parts = frames(text);
  let k = 0;
  const step = () => {
    drawQR($('req-qr'), parts[k]);
    $('req-frame').textContent = parts.length === 1 ? '' : `frame ${k + 1} of ${parts.length}`;
    k = (k + 1) % parts.length;
  };
  step();
  if (parts.length > 1) frameTimer = setInterval(step, 350);
}
function stopFrames() { if (frameTimer) clearInterval(frameTimer); frameTimer = null; }

// ---- camera ----
let camStream = null, camWant = null;
async function scan(want) {
  camWant = want;
  $('cam-card').classList.remove('hidden');
  // On a phone the panel is above the steps: bring the preview into view, or
  // it runs where nobody can see it and aiming is guesswork.
  $('cam-card').scrollIntoView({ behavior: 'smooth', block: 'nearest' });
  if (!camStream) {
    camStream = await navigator.mediaDevices.getUserMedia({ video: { facingMode: 'environment' } });
    const v = $('cam');
    v.srcObject = camStream;
    await v.play();
    const c = document.createElement('canvas');
    const g = c.getContext('2d', { willReadFrequently: true });
    const tick = () => {
      if (!camStream) return;
      if (v.videoWidth) {
        c.width = v.videoWidth; c.height = v.videoHeight;
        g.drawImage(v, 0, 0);
        const r = jsQR(g.getImageData(0, 0, c.width, c.height).data, c.width, c.height);
        if (r && r.data && camWant) {
          const w = camWant;
          camWant = null;
          stopCam();
          w(r.data);
          return;
        }
      }
      requestAnimationFrame(tick);
    };
    tick();
  }
}
function stopCam() {
  if (camStream) camStream.getTracks().forEach((t) => t.stop());
  camStream = null;
  $('cam-card').classList.add('hidden');
}


// ---- pairing ----
// A new pairing never replaces a vault that still holds anything: that
// takes Forget first, which asks. Forget keeps the vault, as it was, in one
// slot per signer; pairing that signer again brings that copy back, always
// the latest, with everything in it (ecash, unfinished swaps, payments,
// history, tip jar). It comes back under a new pairing id, so nothing
// started before it was forgotten can write to it.
function pair(text) {
  const t = text.trim();
  const m = /^\[([0-9a-f]{8})\/129373h\/10h\/0h\/0h\](xpub[1-9A-HJ-NP-Za-km-z]{100,112})$/.exec(t);
  if (!m) throw new Error('that is not a KISS Signer ecash key');
  HDKey.fromExtendedKey(m[2]);   // throws on a bad checksum
  const cur = load();
  if (cur && cur.xpub === t) { tidy(); render(); return; }
  if (holds(cur)) throw new Refusal('this page still holds another signer\'s vault; forget it first');
  // Another signer's vault with no ecash in it can still hold a tip jar key
  // or payment history: it goes to that signer's slot, as Forget would.
  if (cur) localStorage.setItem(FORGOTTEN + cur.xpub, JSON.stringify(cur));
  stopJar();
  clearSignerViews();
  const raw = readRaw(FORGOTTEN + t);
  let back = null;
  if (raw !== null) {
    try { back = JSON.parse(raw); } catch { /* below */ }
    if (!wellFormed(back) || back.xpub !== t) throw Object.assign(new Unreadable('the copy of this signer\'s forgotten vault cannot be read'), { raw });
  }
  if (back) {
    back = { ...back, gen: rand32(), next: Number.isSafeInteger(back.next) ? back.next : 0,
      kpin: back.kpin || {}, ...Object.fromEntries(LISTS.map((k) => [k, back[k] || []])) };
    for (const x of [...back.outbox, ...back.sent]) x.key = x.key || 'tok:' + tag(x.token);
  }
  save(back || fresh(t, m[1]));
  tidy();
  render();
  if (back) msg('recv-msg', 'This signer\'s vault, as it was when you forgot it, is back.', 'ok');
  listenJar();
  recoverPending(st.gen).catch(() => { /* listed as unfinished */ });
}
// Forget keeps the vault in this signer's slot (the newest copy, which
// already holds everything an earlier copy did) and refuses to touch a
// vault it cannot read.
function forget() {
  let cur;
  try { cur = load(); } catch (e) {
    alert(`${e.message}. Nothing was removed.`);
    return;
  }
  if (holds(cur) && !confirm('This vault still holds ecash or unfinished swaps. Forget it anyway? Your signer can still spend its ecash, and pairing this signer again in this browser brings the vault back.')) return;
  try {
    if (cur) localStorage.setItem(FORGOTTEN + cur.xpub, JSON.stringify(cur));
    localStorage.removeItem(STORE);
  } catch (e) {
    alert(`Could not forget this vault in this browser (${e.message || e}). Nothing was removed.`);
    return;
  }
  stopJar();
  clearSignerViews();
  st = null;
  render();
}
// Nothing of the old signer stays on screen once it is forgotten or replaced.
function clearSignerViews() {
  openReq = null;
  stopFrames();
  for (const id of ['req-box', 'jar-box', 'token-box', 'next-box']) $(id).classList.add('hidden');
  for (const id of ['jar-qr', 'jar-qr-inline', 'req-qr', 'token-qr']) $(id).width = 0;
  for (const id of ['jar-text', 'req-text', 'token-out', 'reply-in']) $(id).value = '';
  shownId = shownToken = null;
}

// ---- view ----
function groupBy() {
  const g = new Map();
  for (const p of st.proofs) { if (!g.has(p.counter)) g.set(p.counter, []); g.get(p.counter).push(p); }
  return g;
}
function msg(id, text, cls) {
  const e = $(id);
  e.textContent = text;
  e.className = cls || 'mut';
}
function render() {
  const paired = owner && !!st;
  for (const id of ['vault-card', 'recv-card', 'send-card', 'jar-card']) $(id).classList.toggle('hidden', !paired);
  $('pair-card').classList.toggle('hidden', !owner || paired);
  if (!paired) return;
  // The headline is what a spend can use: the same rule chooseInputs
  // follows. Ecash the signer would refuse, from an expired keyset, or that
  // the mint last called pending is shown apart, beside its key.
  const bal = st.proofs.filter(spendable).reduce((s, p) => s + p.amount, 0);
  $('bal').textContent = bal;
  $('bal-unit').textContent = bal === 1 ? 'sat' : 'sats';
  $('mint-url').textContent = MINT;
  $('fp').textContent = st.fp;
  const sum = (ps) => ps.reduce((s, p) => s + p.amount, 0);
  const g = [...groupBy().entries()].map(([c, ps]) => {
    const bad = sum(ps.filter((p) => p.bad));
    const exp = sum(ps.filter((p) => !p.bad && verified.has(p.id) && expired(verified.get(p.id))));
    const wait = sum(ps.filter((p) => !p.bad && mintPending.has(p.secret)));
    const notes = [bad && `${bad} your signer cannot spend`, exp && `${exp} from an expired keyset`, wait && `${wait} the mint calls pending`].filter(Boolean);
    return `key ${c}: ${sum(ps)}${notes.length ? ` (${notes.join('; ')})` : ''}`;
  });
  $('groups').textContent = g.length ? g.join(', ') : 'nothing yet';
  // Every pending record shows here except one this tab is sending right
  // now: a record is unfinished from the moment it is saved.
  const un = st.pending.filter((r) => !inFlight.has(r.id));
  const parts = [];
  if (un.length) parts.push(`${un.length} unfinished swap${un.length > 1 ? 's' : ''}, kept and tried again on every load. Oldest: ${un[0].note || 'not finished yet'}`);
  if (st.archive.length) parts.push(`${st.archive.length} set aside, still kept and looked for at the mint.`);
  if (st.aside.length) parts.push(`${sats(sum(st.aside))} held apart because the mint calls it spent; kept, and it comes back if the mint calls it unspent.`);
  $('stuck-text').textContent = parts.join(' ');
  $('stuck-box').classList.toggle('hidden', !parts.length);
  $('stuck-aside').classList.toggle('hidden', !un.length);
  $('stuck-resume').classList.toggle('hidden', !st.archive.length);
  $('stuck-retry').classList.toggle('hidden', !parts.length);
  renderOutbox();
}

const guard = (id, f) => async (...a) => {
  try { await f(...a); } catch (e) { msg(id, e.message || String(e), 'bad'); }
};
$('pair-btn').onclick = guard('pair-msg', () => pair($('xpub-in').value));
$('pair-scan').onclick = guard('pair-msg', () => scan(guard('pair-msg', pair)));
$('unpair').onclick = forget;
$('mint-btn').onclick = guard('recv-msg', mintTest);
$('token-btn').onclick = guard('recv-msg', receiveToken);
$('send-btn').onclick = guard('send-msg', makeRequest);
$('reply-btn').onclick = guard('send-msg', () => submitReply($('reply-in').value));
$('reply-scan').onclick = guard('send-msg', () => scan(guard('send-msg', submitReply)));
$('req-cancel').onclick = () => { openReq = null; stopFrames(); stopCam(); $('req-box').classList.add('hidden'); msg('send-msg', ''); };
$('token-done').onclick = guard('send-msg', sentIt);
$('token-next').onclick = guard('send-msg', showNext);
$('stuck-retry').onclick = guard('recv-msg', () => recoverPending(st.gen));
$('stuck-aside').onclick = guard('recv-msg', setAside);
$('stuck-resume').onclick = guard('recv-msg', resume);
$('cam-stop').onclick = stopCam;
$('jar-btn').onclick = guard('jar-msg', showJar);
$('jar-hide').onclick = () => $('jar-box').classList.add('hidden');
drawQR($('key-qr'), frames(KEY_REQUEST)[0]);

// The right hand panel follows the steps: each of its cards shows while the
// step it belongs to is open, so no toggle elsewhere has to know it exists.
// The camera takes the panel when it is on, since by then the signer has
// already read whatever QR was there.
function syncStage() {
  const on = (id) => !$(id).classList.contains('hidden');
  const cam = on('cam-card');
  const show = {
    'st-key': on('pair-card') && !cam,
    'st-req': on('req-box') && on('send-card') && !cam,
    'st-token': on('token-box') && on('send-card') && !on('req-box') && !cam,
    'st-jar': on('jar-box') && on('jar-card') && !cam,
  };
  let opened = null;
  for (const [id, v] of Object.entries(show)) {
    if (v && $(id).classList.contains('hidden')) opened = id;
    $(id).classList.toggle('hidden', !v);
  }
  $('st-idle').classList.toggle('hidden', cam || Object.values(show).some(Boolean));
  // On a phone the panel sits above the steps, out of sight of the button
  // that filled it: bring a new QR to the screen.
  if (opened && opened !== 'st-jar' && narrow.matches) $(opened).scrollIntoView({ behavior: 'smooth', block: 'start' });
}
const narrow = matchMedia('(max-width: 899px)');
const stageWatch = new MutationObserver(syncStage);
for (const id of ['pair-card', 'send-card', 'req-box', 'token-box', 'cam-card', 'jar-card', 'jar-box'])
  stageWatch.observe($(id), { attributes: true, attributeFilter: ['class'] });
render();
syncStage();

// ---- one tab at a time ----
// The vault runs in the one tab that holds this lock (Web Locks, shared by
// every tab of this site), for as long as that tab is open. Another tab
// shows only a note, so two copies of the page never act on one vault. A
// reload can briefly overlap the closing page, hence the few retries. If
// the page cannot start, it lets the lock go and says why.
function start() {
  owner = true;
  $('tab-card').classList.add('hidden');
  try { tidy(); } catch (e) {
    if (!(e instanceof Unreadable)) throw e;
    // Kept as it is: nothing replaces or removes it, and its text is shown
    // so it can be copied out.
    blocked(`${e.message[0].toUpperCase()}${e.message.slice(1)}. Nothing was changed: the page will not replace or remove it. Its text is below, to copy somewhere safe.`);
    $('tab-raw').value = e.raw || '';
    $('tab-raw').classList.toggle('hidden', !e.raw);
    return false;
  }
  if (st && st.outbox.length) shownId = st.outbox[0].id;
  render();
  if (!st) return;
  listenJar();
  recoverPending(st.gen).catch((e) => msg('recv-msg', `An unfinished swap is still not done (${why(e)}). ${KEPT}`, 'bad'));
}
function blocked(text) {
  owner = false;
  $('tab-msg').textContent = text;
  $('tab-card').classList.remove('hidden');
  try { render(); } catch { /* the note is what matters */ }
}
function takeLock(tries) {
  let release = null;
  const held = new Promise((r) => { release = r; });
  navigator.locks.request('kiss-ecash-vault', { ifAvailable: true }, (lock) => {
    if (!lock) {
      if (tries > 0) setTimeout(() => takeLock(tries - 1), 700);
      else blocked('This vault is already open in another tab or window. Use that one, or close it and reload this page.');
      return undefined;
    }
    try { if (start() === false) release(); } catch (e) {
      release();
      blocked(`The vault could not start in this browser (${e.message || e}). Reload the page to try again.`);
    }
    return held;   // kept until the tab closes
  }).catch((e) => blocked(`This browser would not let the vault take its tab lock (${e.message || e}). Open the page in Brave or Chrome.`));
}
if (navigator.locks && navigator.locks.request) takeLock(4);
else blocked('This browser cannot keep two tabs of the vault apart. Open the page in Brave or Chrome.');
