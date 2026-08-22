#!/usr/bin/env python3
"""Offline regression tests for nvs_seed_scan.py.

The fixture is an ESP-IDF v2 NVS page built byte-for-byte here so CI needs no
esptool or NVS generator.  Sensitive entries are marked ERASED in the bitmap
while their physical bytes remain intact, matching nvs_erase_key().

The damage cases matter as much as the clean one.  A forensic tool that stops
answering when a checksum byte moves reports "erased" for seed material that is
still physically on the chip, and that answer feeds an eFuse checklist.
"""

import hashlib
import hmac
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import nvs_seed_scan as scan


MNEMONIC = (
    b"abandon abandon abandon abandon abandon abandon abandon abandon "
    b"abandon abandon abandon about"
)
RECOVERY_ID = "C557EEC878DF"
NKEY = bytes(range(32))
WRONG_NKEY = bytes(reversed(range(32)))
WBLOB = bytes.fromhex(
    "4b49535353443031101112131415161718191a1b1c1d1e1f60000000"
    "19dc922bc1ec93b01b36d68e40af09f17303fc9b8a3cf7646b4fd18d"
    "fdde4bda9f676348c92be2c06879200c01f2c4d5b658af3f797978f8"
    "4b27fa0dacd554bc127e976ac5bb726fb1081601e1455697ff455321f5"
    "b9686a6c16c9263e1c3c979a6da07ae2a4e1fed81dc901503d8f493f"
    "74e55e73c1206066025521fa387bfb"
)


class PageBuilder:
    def __init__(self):
        self.page = bytearray(b"\xff" * scan.PAGE_SIZE)
        struct.pack_into("<I", self.page, 0, 0xFFFFFFFE)  # active
        struct.pack_into("<I", self.page, 4, 0)
        self.page[8] = 0xFE  # format version 2
        struct.pack_into("<I", self.page, 28, scan.nvs_crc(self.page[4:28]))

    def _state(self, slot, bits=0b00):
        byte_at = scan.ENTRY_SIZE + slot // 4
        shift = (slot % 4) * 2
        mask = 0b11 << shift
        self.page[byte_at] = (self.page[byte_at] & ~mask) | (bits << shift)

    @staticmethod
    def _metadata(namespace, entry_type, span, chunk, key, data):
        raw = bytearray(32)
        raw[0:4] = bytes((namespace, entry_type, span, chunk))
        raw[8:24] = key.encode("ascii") + b"\0" * (16 - len(key))
        raw[24:32] = data
        struct.pack_into("<I", raw, 4, scan.nvs_crc(raw[:4] + raw[8:]))
        return raw

    def variable(self, slot, key, payload, entry_type=scan.TYPE_BLOB_DATA, chunk=0):
        span = 1 + (len(payload) + 31) // 32
        data = struct.pack("<HHI", len(payload), 0, scan.nvs_crc(payload))
        raw = self._metadata(1, entry_type, span, chunk, key, data)
        at = scan.PAGE_HEADER_SIZE + slot * scan.ENTRY_SIZE
        self.page[at : at + 32] = raw
        padded = payload + b"\xff" * ((span - 1) * 32 - len(payload))
        self.page[at + 32 : at + span * 32] = padded
        for used in range(slot, slot + span):
            self._state(used)  # erased, but bytes physically present
        return slot + span

    def index(self, slot, key, size, chunk_count=1, chunk_start=0):
        data = struct.pack("<IBB2s", size, chunk_count, chunk_start, b"\xff\xff")
        raw = self._metadata(1, scan.TYPE_BLOB_INDEX, 1, 0xFF, key, data)
        at = scan.PAGE_HEADER_SIZE + slot * scan.ENTRY_SIZE
        self.page[at : at + 32] = raw
        self._state(slot)
        return slot + 1

    def blob_v2(self, slot, key, payload, chunk=0):
        slot = self.variable(slot, key, payload, chunk=chunk)
        return self.index(slot, key, len(payload), 1, chunk)

    def bytes(self):
        return bytes(self.page)


def fixture(*, include_key=True, blob=WBLOB, include_plaintext=False, legacy=False):
    page = PageBuilder()
    slot = 0
    if include_key:
        kind = scan.TYPE_BLOB_LEGACY if legacy else scan.TYPE_BLOB_DATA
        if legacy:
            slot = page.variable(slot, "nkey", NKEY, entry_type=kind, chunk=0xFF)
        else:
            slot = page.blob_v2(slot, "nkey", NKEY)

        # A second stale generation exercises authenticated cross-pairing.
        slot = page.blob_v2(slot, "nkey", WRONG_NKEY)

    if legacy:
        slot = page.variable(
            slot, "wblob", blob, entry_type=scan.TYPE_BLOB_LEGACY, chunk=0xFF
        )
    else:
        slot = page.blob_v2(slot, "wblob", blob)
    if include_plaintext:
        page.variable(slot, "words", MNEMONIC + b"\0", entry_type=scan.TYPE_STRING)
    return page.bytes()


# ---- damage, applied to a finished page -------------------------------
# Each of these targets exactly one metadata field and leaves every payload
# byte alone, so what is left is what a bit flip on real flash leaves: the
# record walk goes blind, the seed material does not move.
def _records(raw):
    return scan.parse_nvs_records(raw, 0)[0]


def damage_entry_crcs(raw):
    """Corrupt the entry CRC of every record the walk can see."""
    page = bytearray(raw)
    for record in _records(raw):
        page[record.address + 4] ^= 0xFF
    return bytes(page)


def damage_data_crcs(raw):
    """Corrupt the payload CRC while keeping each entry itself well formed."""
    page = bytearray(raw)
    for record in _records(raw):
        if record.kind not in ("chunk", "legacy"):
            continue
        page[record.address + 28] ^= 0xFF
        entry = bytes(page[record.address : record.address + 32])
        struct.pack_into(
            "<I", page, record.address + 4, scan.nvs_crc(entry[:4] + entry[8:])
        )
    return bytes(page)


def blank_indexes(raw):
    """Erase the BLOB_INDEX records, leaving their data chunks behind."""
    page = bytearray(raw)
    for record in _records(raw):
        if record.kind == "index":
            page[record.address : record.address + 32] = b"\xff" * 32
    return bytes(page)


def checked_result(raw):
    _, indexes = scan.load_wordlist(scan.default_wordlist())
    return scan.scan_dump(raw, 0x1A000, indexes)


def main():
    # The production path authenticates, decrypts, checks PKCS#7, and validates
    # BIP39 before emitting the same ID as the old plaintext scan.
    result = checked_result(fixture(include_plaintext=True))
    assert set(result.recoveries) == {RECOVERY_ID}
    assert set(result.plaintext) == {RECOVERY_ID}
    assert result.recoveries[RECOVERY_ID].states == {"erased"}
    rendered = scan.render_result(result, "NVS test", "selftest")
    assert RECOVERY_ID in rendered
    assert MNEMONIC.decode() not in rendered
    assert NKEY.hex() not in rendered.lower()
    assert "wallet" not in rendered.lower()

    # Missing key and a tag damaged while retaining valid NVS record CRCs must
    # not be called recoverable.
    assert not checked_result(fixture(include_key=False)).recoveries
    damaged = bytearray(WBLOB)
    damaged[-1] ^= 1
    assert not checked_result(fixture(blob=bytes(damaged))).recoveries

    # A blob authenticated for the removable-card domain cannot cross into the
    # internal NVS domain.  The ciphertext need not be touched: domain-separated
    # MAC rejection happens before decryption in both firmware and scanner.
    card_blob = bytearray(WBLOB)
    card_mac_key = hmac.new(NKEY, b"kiss-sd-mac-v1", hashlib.sha256).digest()
    card_blob[-32:] = hmac.new(card_mac_key, card_blob[:-32], hashlib.sha256).digest()
    assert not checked_result(fixture(blob=bytes(card_blob))).recoveries

    # Pre-v2 one-record blobs remain recoverable and are deduplicated exactly as
    # v2 data/index pairs are.
    legacy = checked_result(fixture(legacy=True))
    assert set(legacy.recoveries) == {RECOVERY_ID}

    # Random/encrypted bytes cannot silently produce a clean forensic verdict.
    opaque = checked_result(bytes(range(256)) * 16)
    opaque_text = scan.render_result(opaque, "NVS test", "opaque")
    assert "inconclusive" in opaque_text

    # ---- damaged metadata, intact seed material ------------------------
    # The whole point of the raw fallback: every one of these hides the records
    # and none of them touches a payload byte, so "not recoverable" would be a
    # lie about what is on the chip.
    blinded = damage_entry_crcs(fixture())
    assert not _records(blinded), "the record walk must be blind here"
    raw_only = checked_result(blinded)
    assert set(raw_only.recoveries) == {RECOVERY_ID}
    assert raw_only.recoveries[RECOVERY_ID].states == {"raw"}
    raw_text = scan.render_result(raw_only, "NVS test", "raw")
    assert MNEMONIC.decode() not in raw_text
    assert NKEY.hex() not in raw_text.lower()
    assert "wallet" not in raw_text.lower()

    # A payload CRC flip leaves the entries well formed and the walk still
    # refuses the bytes, which is the same blindness by a different route.
    assert _records(damage_data_crcs(fixture())), "entries must still parse"
    assert set(checked_result(damage_data_crcs(fixture())).recoveries) == {RECOVERY_ID}

    # An index lost to compaction leaves its data chunk addressable.
    assert set(checked_result(blank_indexes(fixture())).recoveries) == {RECOVERY_ID}

    # Nothing readable at all: no index, no usable CRC anywhere.
    wrecked = damage_entry_crcs(blank_indexes(fixture()))
    assert set(checked_result(wrecked).recoveries) == {RECOVERY_ID}

    # ...and the sweep must not MANUFACTURE one.  With the key gone, every
    # window in the page gets tried against the blob and the tag rejects all of
    # them, damaged metadata or not.
    assert not checked_result(damage_entry_crcs(fixture(include_key=False))).recoveries
    assert NKEY not in fixture(include_key=False), "fixture must not leak the key"

    # A short or misaligned read has no page geometry, and is still a dump full
    # of bytes.  Refusing to look at it would be the same false clean answer.
    stub = fixture()[:-7]
    try:
        scan.parse_nvs_records(stub, 0)
        raise AssertionError("a non-page-multiple dump must not parse as pages")
    except scan.ScanError:
        pass
    short = checked_result(stub)
    assert set(short.recoveries) == {RECOVERY_ID}
    assert "page geometry unreadable" in scan.render_result(short, "NVS test", "short")

    print("PASS: erased NVS nkey+wblob recovery -> " + RECOVERY_ID)
    print("PASS: recovery survives damaged NVS metadata (raw fallback)")


if __name__ == "__main__":
    main()
