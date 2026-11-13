# Vendored for the ecash vault page

Copied from the npm registry, unmodified, so the page loads nothing from
another site.

- cashu-ts 4.13.1 (MIT), with the files it imports from @noble/curves,
  @noble/hashes, @scure/base and @scure/bip32 (MIT), each with its licence.
  Only the files reachable from `lib/cashu-ts.es.js` are here; the page's
  import map in `../index.html` points the package names at them.
- qrcode-generator 1.4.4 (MIT, licence in the file header): draws QR codes.
- jsQR 1.4.0 (Apache 2.0, `jsqr-LICENSE`): reads QR codes from the camera.
