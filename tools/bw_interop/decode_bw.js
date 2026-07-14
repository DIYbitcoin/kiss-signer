// Decode UR frames the way BlueWallet's scanner does (@ngraveio URDecoder,
// receivePart until complete), then unwrap crypto-psbt and print base64.
const { URDecoder } = require('@ngraveio/bc-ur');
const { CryptoPSBT } = require('@keystonehq/bc-ur-registry');

let input = '';
process.stdin.on('data', d => (input += d));
process.stdin.on('end', () => {
  const dec = new URDecoder();
  for (const line of input.split('\n')) {
    const part = line.trim();
    if (!part) continue;
    dec.receivePart(part);
    if (dec.isComplete()) break;
  }
  if (!dec.isComplete()) { console.error('NOT COMPLETE'); process.exit(1); }
  if (!dec.isSuccess()) { console.error(dec.resultError()); process.exit(1); }
  const ur = dec.resultUR();
  if (ur.type !== 'crypto-psbt') { console.error('type=' + ur.type); process.exit(1); }
  const psbt = CryptoPSBT.fromCBOR(ur.cbor).getPSBT();
  process.stdout.write(psbt.toString('base64'));
});
