// What the test builds page checks in an image before it writes a byte,
// kept apart from the page so it can be run on its own against the files.

export const revLabel = (rev) => `v${Math.floor(rev / 100)}.${rev % 100}`;

// The chip range an image was built for, from the header the ROM and the
// bootloader read: min_chip_rev_full at bytes 15-16, max at 17-18.
function header(bytes, offset, what) {
  if (offset + 24 > bytes.length || bytes[offset] !== 0xe9) {
    throw new Error(`The test build has no ${what} where it should be. Nothing was erased.`);
  }
  const v = new DataView(bytes.buffer, bytes.byteOffset + offset, 24);
  if (v.getUint16(12, true) !== 18) {
    throw new Error("The test build is not for an ESP32-P4. Nothing was erased.");
  }
  return { min: v.getUint16(15, true), max: v.getUint16(17, true) };
}

// The image is one merged file flashed at 0: bootloader at 0x2000, the
// partition table at 0x10000, and the app wherever the table puts ota_0.
export function checkImage(bytes, board, rev) {
  const boot = header(bytes, 0x2000, "bootloader");
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let app;
  for (let p = 0x10000; p + 32 <= Math.min(bytes.length, 0x11000); p += 32) {
    if (view.getUint16(p, true) !== 0x50aa) break;
    if (bytes[p + 2] === 0 && bytes[p + 3] === 0x10) app = view.getUint32(p + 4, true);
  }
  if (app === undefined) throw new Error("The test build has no app slot. Nothing was erased.");
  const main = header(bytes, app, "app");
  for (const h of [boot, main]) {
    if (rev < h.min || rev > h.max) {
      throw new Error(`This test build runs on ${revLabel(h.min)} to ${revLabel(h.max)}, and your chip is ${revLabel(rev)}. Nothing was erased.`);
    }
  }
  if (view.getUint32(app + 32, true) !== 0xabcd5432) {
    throw new Error("The test build has no app description. Nothing was erased.");
  }
  const name = new TextDecoder().decode(bytes.subarray(app + 80, app + 112)).split("\0")[0];
  if (name !== `${board.id}_kiss_bringup`) {
    throw new Error(`That file is the build for ${name.replace("_kiss_bringup", "")}, not for this board. Nothing was erased.`);
  }
}

export async function sha256(bytes) {
  const d = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  return [...d].map((b) => b.toString(16).padStart(2, "0")).join("");
}
