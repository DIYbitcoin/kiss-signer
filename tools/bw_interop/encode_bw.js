// Emit UR frames for a PSBT EXACTLY like BlueWallet displays them:
// new CryptoPSBT(psbt).toUREncoder(175), one pure fragment per line
// (BlueWallet blue_modules/ur/index.js encodeURv2, capacity default 175).
const fs = require('fs');
const { CryptoPSBT } = require('@keystonehq/bc-ur-registry');

const psbt = fs.readFileSync(process.argv[2]); // binary PSBT file
const enc = new CryptoPSBT(psbt).toUREncoder(175);
for (let i = 1; i <= enc.fragmentsLength; i++) console.log(enc.nextPart());
