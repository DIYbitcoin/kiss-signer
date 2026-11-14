# Vendored for the ecash vault page

Copied from the npm registry, unmodified, so the page loads nothing from
another site.

- cashu-ts 4.13.1 (MIT), with the files it imports from @noble/curves,
  @noble/hashes, @scure/base and @scure/bip32 (MIT), each with its licence.
  Only the files reachable from `lib/cashu-ts.es.js` are here; the page's
  import map in `../index.html` points the package names at them.
- nostr-tools 2.25.5 (Unlicense, `nostr-tools/LICENSE`): only `pool.js`,
  `nip59.js`, `pure.js` and `nip19.js` from
  `lib/esm`, which each carry their own copy of what they share. They
  receive tip jar deliveries (NIP-17). Their imports from @noble/curves,
  @noble/hashes and @scure/base resolve to the copies above (2.4.0, the
  same major version as the 2.0.1 they pin), plus `noble-hashes/hkdf.js`
  from that same 2.4.0.
- @noble/ciphers 2.1.1 (MIT, `noble-ciphers/LICENSE`): `chacha.js`,
  `utils.js`, `_arx.js`, `_poly1305.js`, for NIP-44 encryption.
- qrcode-generator 1.4.4 (MIT, licence in the file header): draws QR codes.
- jsQR 1.4.0 (Apache 2.0, `jsqr-LICENSE`): reads QR codes from the camera.
