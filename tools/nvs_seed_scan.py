#!/usr/bin/env python3
"""Forensically scan a raw ESP-IDF NVS dump for signer recovery material.

Unlike the NVS API, this reads entries whose bitmap state is ERASED.  That is
the state left by nvs_erase_key(): the value is gone from the live key/value
view, but its bytes can remain on flash until the page is physically erased.

The scanner recognizes the ESP-IDF v2 BLOB_DATA/BLOB_INDEX representation and
the older one-record BLOB representation.  It pairs every recoverable ``nkey``
with every ``wblob``, authenticates the pair, decrypts it in memory, validates
the English BIP39 checksum, and prints only a short recovery ID.  It never
prints the mnemonic.

When that metadata is damaged it falls back to reading the flash directly: the
sealed blob is found by its magic and the device key by sweeping windows, with
the blob's own HMAC tag deciding which pairing is real.  A tool that answers
"erased" because one checksum byte moved would hand back a clean verdict on
seed material still sitting on the chip.

Only Python's standard library is used.  AES-CBC is provided by the libcrypto
that Python/OpenSSL already uses on the supported macOS/Linux hardware-check
hosts; keys are passed through ctypes, never process arguments.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import hashlib
import hmac
import itertools
import os
import re
import struct
import sys
import zlib
from dataclasses import dataclass


PAGE_SIZE = 4096
ENTRY_SIZE = 32
PAGE_HEADER_SIZE = 64
ENTRIES_PER_PAGE = 126

TYPE_STRING = 0x21
TYPE_BLOB_LEGACY = 0x41
TYPE_BLOB_DATA = 0x42
TYPE_BLOB_INDEX = 0x48

TARGET_KEYS = {"nkey", "wblob"}
VALID_PAGE_STATES = {
    0xFFFFFFFF,  # empty
    0xFFFFFFFE,  # active
    0xFFFFFFFC,  # full
    0xFFFFFFF8,  # erasing
    0x00000000,  # corrupted
}
ENTRY_STATES = {0b11: "empty", 0b10: "written", 0b00: "erased"}

SEED_MAGIC = b"KISSSD01"
SEED_HEADER_LEN = 28
SEED_TAG_LEN = 32
SEED_MAX_CIPHERTEXT = 272
NVS_ENC_INFO = b"kiss-nvs-enc-v1"
NVS_MAC_INFO = b"kiss-nvs-mac-v1"

VALID_WORD_COUNTS = {12, 15, 18, 21, 24}
MNEMONIC_RE = re.compile(
    rb"(?<![a-z])(?:[a-z]{3,8} ){11,23}[a-z]{3,8}(?![a-z])"
)
MAX_CHUNK_COMBINATIONS = 4096

# The raw sweep, for dumps whose NVS metadata is damaged. One flipped bit in an
# entry CRC, a stored data CRC or an index record hides a payload that is still
# physically on the page, and a forensic tool that answers "erased" because a
# checksum moved is worse than no tool at all.
#
# It is safe to generate candidates from nothing but position because the blob
# carries a 32-byte HMAC tag: a wrong pairing cannot survive it, so the sweep
# proposes and the tag disposes. Measured at ~8ms per 1024 windows, so the two
# ranges nvs_seed_check.sh reads (0x6000 of nvs, 0x8000 of the legacy layout)
# sweep byte by byte in well under a second. Above the bound a dump is a whole
# flash image rather than a partition, and the stride drops to the NVS entry
# alignment a payload always lands on -- reported, never silent.
RAW_FULL_SCAN_MAX = 1 << 20
RAW_COARSE_STRIDE = ENTRY_SIZE


class ScanError(RuntimeError):
    """The dump cannot be scanned reliably."""


class CryptoUnavailable(ScanError):
    """No usable libcrypto was found for authenticated decryption."""


@dataclass(frozen=True)
class NVSRecord:
    key: str
    namespace: int
    kind: str
    state: str
    address: int
    payload: bytes = b""
    chunk_index: int = 0xFF
    total_size: int = 0
    chunk_count: int = 0
    chunk_start: int = 0


@dataclass(frozen=True)
class BlobCandidate:
    data: bytes
    records: tuple[NVSRecord, ...]


@dataclass
class Recovery:
    recovery_id: str
    word_count: int
    key_addresses: set[int]
    blob_addresses: set[int]
    states: set[str]


@dataclass
class ScanResult:
    size: int
    base: int
    blank: bool
    recognized_pages: int
    records: list[NVSRecord]
    plaintext: dict[str, tuple[int, set[int]]]
    recoveries: dict[str, Recovery]
    combinations_truncated: bool
    pages_parsed: bool = True
    page_error: str = ""
    raw_stride: int = 1


def nvs_crc(data: bytes) -> int:
    """ESP-IDF NVS CRC32 (same initial value used by Espressif's parser)."""
    return zlib.crc32(data, 0xFFFFFFFF) & 0xFFFFFFFF


def _entry_state(bitmap: bytes, slot: int) -> str:
    bits = (bitmap[slot // 4] >> ((slot % 4) * 2)) & 0x03
    return ENTRY_STATES.get(bits, "invalid")


def _decode_key(raw: bytes) -> str | None:
    key = raw.split(b"\0", 1)[0]
    if not key or len(key) > 15:
        return None
    try:
        text = key.decode("ascii")
    except UnicodeDecodeError:
        return None
    if any(ord(c) < 0x20 or ord(c) > 0x7E for c in text):
        return None
    return text


def _valid_entry_crc(raw: bytes) -> bool:
    if len(raw) != ENTRY_SIZE:
        return False
    stored = struct.unpack_from("<I", raw, 4)[0]
    return stored == nvs_crc(raw[:4] + raw[8:])


def _page_recognized(page: bytes) -> bool:
    if page == b"\xff" * PAGE_SIZE:
        return True
    state = struct.unpack_from("<I", page, 0)[0]
    stored = struct.unpack_from("<I", page, 28)[0]
    return state in VALID_PAGE_STATES and stored == nvs_crc(page[4:28])


def parse_nvs_records(dump: bytes, base: int = 0) -> tuple[list[NVSRecord], int]:
    """Return intact target records, including entries marked erased."""
    if len(dump) % PAGE_SIZE:
        raise ScanError(
            f"dump length {len(dump)} is not a multiple of the NVS page size"
        )

    records: list[NVSRecord] = []
    recognized_pages = 0
    for page_off in range(0, len(dump), PAGE_SIZE):
        page = dump[page_off : page_off + PAGE_SIZE]
        if _page_recognized(page):
            recognized_pages += 1
        bitmap = page[ENTRY_SIZE : 2 * ENTRY_SIZE]

        # Inspect every slot independently.  A damaged predecessor must not hide
        # a later intact erased record; the entry CRC keeps child payload bytes
        # from being mistaken for metadata.
        for slot in range(ENTRIES_PER_PAGE):
            at = PAGE_HEADER_SIZE + slot * ENTRY_SIZE
            raw = page[at : at + ENTRY_SIZE]
            if not _valid_entry_crc(raw):
                continue

            namespace, entry_type, span, chunk_index = raw[:4]
            key = _decode_key(raw[8:24])
            if key not in TARGET_KEYS:
                continue
            state = _entry_state(bitmap, slot)
            address = base + page_off + at

            if entry_type == TYPE_BLOB_INDEX:
                if span != 1:
                    continue
                total_size = struct.unpack_from("<I", raw, 24)[0]
                chunk_count = raw[28]
                chunk_start = raw[29]
                if not total_size or not chunk_count:
                    continue
                records.append(
                    NVSRecord(
                        key,
                        namespace,
                        "index",
                        state,
                        address,
                        total_size=total_size,
                        chunk_count=chunk_count,
                        chunk_start=chunk_start,
                    )
                )
                continue

            if entry_type not in (TYPE_BLOB_DATA, TYPE_BLOB_LEGACY):
                continue
            size = struct.unpack_from("<H", raw, 24)[0]
            expected_span = 1 + (size + ENTRY_SIZE - 1) // ENTRY_SIZE
            if not size or span != expected_span or slot + span > ENTRIES_PER_PAGE:
                continue
            child_at = at + ENTRY_SIZE
            payload = page[child_at : child_at + (span - 1) * ENTRY_SIZE][:size]
            stored_data_crc = struct.unpack_from("<I", raw, 28)[0]
            if stored_data_crc != nvs_crc(payload):
                continue
            records.append(
                NVSRecord(
                    key,
                    namespace,
                    "chunk" if entry_type == TYPE_BLOB_DATA else "legacy",
                    state,
                    address,
                    payload=payload,
                    chunk_index=chunk_index,
                    total_size=size,
                )
            )

    return records, recognized_pages


def _seed_blob_shape(blob: bytes) -> bool:
    if len(blob) < SEED_HEADER_LEN + 16 + SEED_TAG_LEN:
        return False
    if blob[: len(SEED_MAGIC)] != SEED_MAGIC:
        return False
    ciphertext_len = struct.unpack_from("<I", blob, 24)[0]
    return (
        0 < ciphertext_len <= SEED_MAX_CIPHERTEXT
        and ciphertext_len % 16 == 0
        and SEED_HEADER_LEN + ciphertext_len + SEED_TAG_LEN == len(blob)
    )


def _candidate_ok(key: str, data: bytes) -> bool:
    return len(data) == 32 if key == "nkey" else _seed_blob_shape(data)


def blob_candidates(
    records: list[NVSRecord], key: str
) -> tuple[list[BlobCandidate], bool]:
    """Reassemble every plausible generation of one NVS blob key."""
    candidates: list[BlobCandidate] = []
    truncated = False

    # Legacy blobs and single v2 chunks are independently useful.  The latter
    # matters when compaction destroyed an index but left its data record.
    for record in records:
        if record.key == key and record.kind in ("legacy", "chunk"):
            if _candidate_ok(key, record.payload):
                candidates.append(BlobCandidate(record.payload, (record,)))

    chunks: dict[tuple[int, int], list[NVSRecord]] = {}
    for record in records:
        if record.key == key and record.kind == "chunk":
            chunks.setdefault((record.namespace, record.chunk_index), []).append(record)

    for index in records:
        if index.key != key or index.kind != "index":
            continue
        if key == "nkey" and index.total_size != 32:
            continue
        if key == "wblob" and not (
            SEED_HEADER_LEN + 16 + SEED_TAG_LEN
            <= index.total_size
            <= SEED_HEADER_LEN + SEED_MAX_CIPHERTEXT + SEED_TAG_LEN
        ):
            continue

        choices: list[list[NVSRecord]] = []
        for i in range(index.chunk_count):
            chunk_index = (index.chunk_start + i) & 0xFF
            options = chunks.get((index.namespace, chunk_index), [])
            if not options:
                choices = []
                break
            choices.append(options)
        if not choices:
            continue

        combinations = 1
        for options in choices:
            combinations *= len(options)
        if combinations > MAX_CHUNK_COMBINATIONS:
            truncated = True

        for n, combo in enumerate(itertools.product(*choices)):
            if n >= MAX_CHUNK_COMBINATIONS:
                break
            if len({record.address for record in combo}) != len(combo):
                continue
            data = b"".join(record.payload for record in combo)
            if len(data) != index.total_size or not _candidate_ok(key, data):
                continue
            candidates.append(BlobCandidate(data, (index,) + tuple(combo)))

    return candidates, truncated


def dedupe_candidates(candidates: list[BlobCandidate]) -> list[BlobCandidate]:
    """Collapse identical bytes, keeping the FIRST provenance offered.

    HMAC matching later makes false combinations harmless, so this is about
    output rather than correctness: repeated index records must not turn one
    residue into several lines.  Order carries meaning -- callers pass the
    metadata-derived candidates first, so a blob that both the record walk and
    the raw sweep found is reported with the record it really came from,
    against the state NVS really has it in.
    """
    unique: dict[bytes, BlobCandidate] = {}
    for candidate in candidates:
        unique.setdefault(candidate.data, candidate)
    return list(unique.values())


def raw_blob_candidates(dump: bytes, base: int) -> list[BlobCandidate]:
    """Find sealed blobs by their magic, with no NVS metadata involved.

    A sealed blob is at most SDSEED_MAX_BLOB (332) bytes, which fits inside one
    NVS entry span, so its bytes are always contiguous on the page and NVS
    never splits it into chunks.  That makes this fallback complete for wblob:
    every blob a record walk can find is also findable here, whatever happened
    to the metadata around it.
    """
    candidates: list[BlobCandidate] = []
    at = dump.find(SEED_MAGIC)
    while at >= 0:
        limit = SEED_HEADER_LEN + SEED_MAX_CIPHERTEXT + SEED_TAG_LEN
        window = dump[at : at + limit]
        if len(window) >= SEED_HEADER_LEN + 4:
            ciphertext_len = struct.unpack_from("<I", window, 24)[0]
            end = SEED_HEADER_LEN + ciphertext_len + SEED_TAG_LEN
            blob = window[:end]
            if len(blob) == end and _seed_blob_shape(blob):
                record = NVSRecord("wblob", 0, "raw", "raw", base + at)
                candidates.append(BlobCandidate(blob, (record,)))
        at = dump.find(SEED_MAGIC, at + 1)
    return candidates


def raw_key_candidates(dump: bytes, base: int, stride: int):
    """Yield every window that could be a retained 32-byte device key.

    A device key has no magic and no header -- it is 32 bytes of TRNG output --
    so position is the only handle there is.  Constant windows are skipped:
    0xFF is erased flash and 0x00 is a hole, and fill_random() cannot produce
    either, so they are the bulk of a mostly-blank partition and none of them
    is a key.
    """
    for at in range(0, len(dump) - 31, stride):
        window = dump[at : at + 32]
        if window == b"\xff" * 32 or window == b"\0" * 32:
            continue
        record = NVSRecord("nkey", 0, "raw", "raw", base + at)
        yield BlobCandidate(window, (record,))


_LIBCRYPTO = None


def _libcrypto():
    global _LIBCRYPTO
    if _LIBCRYPTO is not None:
        return _LIBCRYPTO
    # find_library shells out to ldconfig/gcc and returns None on a minimal
    # Linux image that has neither, even with libcrypto.so.3 right there, so
    # the well-known sonames are tried by hand before giving up.  Giving up is
    # a hard failure and never a skip: a run that could not decrypt must not
    # be mistaken for a run that found nothing.
    names = [ctypes.util.find_library("crypto")]
    names += ["libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so", "libcrypto.dylib"]
    problem = "libcrypto not found"
    for name in names:
        if not name:
            continue
        try:
            lib = ctypes.CDLL(name)
            break
        except OSError as exc:
            problem = f"cannot load {name} ({exc})"
    else:
        raise CryptoUnavailable(
            f"{problem}; OpenSSL is required for authenticated mnemonic validation"
        )

    void_p = ctypes.c_void_p
    int_p = ctypes.POINTER(ctypes.c_int)
    lib.EVP_CIPHER_CTX_new.argtypes = []
    lib.EVP_CIPHER_CTX_new.restype = void_p
    lib.EVP_CIPHER_CTX_free.argtypes = [void_p]
    lib.EVP_CIPHER_CTX_free.restype = None
    lib.EVP_aes_256_cbc.argtypes = []
    lib.EVP_aes_256_cbc.restype = void_p
    lib.EVP_DecryptInit_ex.argtypes = [void_p, void_p, void_p, void_p, void_p]
    lib.EVP_DecryptInit_ex.restype = ctypes.c_int
    lib.EVP_DecryptUpdate.argtypes = [void_p, void_p, int_p, void_p, ctypes.c_int]
    lib.EVP_DecryptUpdate.restype = ctypes.c_int
    lib.EVP_DecryptFinal_ex.argtypes = [void_p, void_p, int_p]
    lib.EVP_DecryptFinal_ex.restype = ctypes.c_int
    _LIBCRYPTO = lib
    return lib


def aes256_cbc_decrypt(key: bytes, iv: bytes, ciphertext: bytes) -> bytes | None:
    """Decrypt with PKCS#7 validation, keeping the key out of process argv."""
    if len(key) != 32 or len(iv) != 16 or not ciphertext or len(ciphertext) % 16:
        return None
    lib = _libcrypto()
    ctx = lib.EVP_CIPHER_CTX_new()
    if not ctx:
        raise CryptoUnavailable("EVP_CIPHER_CTX_new failed")

    key_buf = ctypes.create_string_buffer(key)
    iv_buf = ctypes.create_string_buffer(iv)
    in_buf = ctypes.create_string_buffer(ciphertext)
    out_buf = ctypes.create_string_buffer(len(ciphertext) + 16)
    first_len = ctypes.c_int(0)
    final_len = ctypes.c_int(0)
    try:
        if lib.EVP_DecryptInit_ex(
            ctx, lib.EVP_aes_256_cbc(), None, key_buf, iv_buf
        ) != 1:
            return None
        if lib.EVP_DecryptUpdate(
            ctx, out_buf, ctypes.byref(first_len), in_buf, len(ciphertext)
        ) != 1:
            return None
        final_at = ctypes.byref(out_buf, first_len.value)
        if lib.EVP_DecryptFinal_ex(ctx, final_at, ctypes.byref(final_len)) != 1:
            return None
        return out_buf.raw[: first_len.value + final_len.value]
    finally:
        ctypes.memset(key_buf, 0, len(key_buf))
        ctypes.memset(iv_buf, 0, len(iv_buf))
        ctypes.memset(out_buf, 0, len(out_buf))
        lib.EVP_CIPHER_CTX_free(ctx)


def load_wordlist(path: str) -> tuple[list[str], dict[str, int]]:
    with open(path, encoding="ascii") as handle:
        words = [line.strip() for line in handle if line.strip()]
    if len(words) != 2048 or len(set(words)) != 2048:
        raise ScanError(f"{path} is not a 2048-word BIP39 wordlist")
    return words, {word: index for index, word in enumerate(words)}


def valid_bip39(mnemonic: str, word_indexes: dict[str, int]) -> bool:
    words = mnemonic.split(" ")
    if len(words) not in VALID_WORD_COUNTS or any(not word for word in words):
        return False
    try:
        indexes = [word_indexes[word] for word in words]
    except KeyError:
        return False

    packed = 0
    for index in indexes:
        packed = (packed << 11) | index
    total_bits = len(words) * 11
    checksum_bits = total_bits // 33
    entropy_bits = total_bits - checksum_bits
    checksum = packed & ((1 << checksum_bits) - 1)
    entropy = (packed >> checksum_bits).to_bytes(entropy_bits // 8, "big")
    expected = hashlib.sha256(entropy).digest()[0] >> (8 - checksum_bits)
    return checksum == expected


def recovery_id(mnemonic: str) -> str:
    """Non-secret comparison token; deliberately not a BIP32 fingerprint."""
    return hashlib.sha256(mnemonic.encode("ascii")).hexdigest()[:12].upper()


def open_sealed(
    key: bytes, mac_key: bytes, blob: bytes, word_indexes: dict[str, int]
) -> tuple[str, int] | None:
    """Authenticate, then decrypt, then check BIP39 -- in that order.

    The firmware verifies the tag over the whole header and ciphertext BEFORE
    it decrypts anything (main/kiss_seed_sd.h), so a wrong device key is a MAC
    failure rather than garbage plaintext, and the same order here is what
    makes the raw sweep cheap: the caller derives one MAC subkey per key
    candidate and the enc subkey is only derived on a tag that matched.
    """
    if len(key) != 32 or not _seed_blob_shape(blob):
        return None
    ciphertext_len = struct.unpack_from("<I", blob, 24)[0]
    signed_len = SEED_HEADER_LEN + ciphertext_len
    expected_tag = hmac.new(mac_key, blob[:signed_len], hashlib.sha256).digest()
    if not hmac.compare_digest(expected_tag, blob[signed_len:]):
        return None

    enc_key = hmac.new(key, NVS_ENC_INFO, hashlib.sha256).digest()
    plaintext = aes256_cbc_decrypt(
        enc_key, blob[len(SEED_MAGIC) : len(SEED_MAGIC) + 16], blob[SEED_HEADER_LEN:signed_len]
    )
    if plaintext is None or not plaintext or len(plaintext) >= 256:
        return None
    try:
        mnemonic = plaintext.decode("ascii")
    except UnicodeDecodeError:
        return None
    if not valid_bip39(mnemonic, word_indexes):
        return None
    return recovery_id(mnemonic), len(mnemonic.split())


def nvs_mac_subkey(key: bytes) -> bytes:
    return hmac.new(key, NVS_MAC_INFO, hashlib.sha256).digest()


def open_seed_blob(
    key: bytes, blob: bytes, word_indexes: dict[str, int]
) -> tuple[str, int] | None:
    """One blob under one key, for callers with a single pair to try."""
    if len(key) != 32:
        return None
    return open_sealed(key, nvs_mac_subkey(key), blob, word_indexes)


def scan_plaintext(
    dump: bytes, base: int, word_indexes: dict[str, int]
) -> dict[str, tuple[int, set[int]]]:
    found: dict[str, tuple[int, set[int]]] = {}
    for match in MNEMONIC_RE.finditer(dump):
        try:
            mnemonic = match.group(0).decode("ascii")
        except UnicodeDecodeError:
            continue
        if not valid_bip39(mnemonic, word_indexes):
            continue
        rid = recovery_id(mnemonic)
        count = len(mnemonic.split())
        if rid not in found:
            found[rid] = (count, set())
        found[rid][1].add(base + match.start())
    return found


def scan_dump(
    dump: bytes, base: int, word_indexes: dict[str, int]
) -> ScanResult:
    # A dump whose page geometry does not parse -- a short or misaligned read --
    # is still a dump full of bytes.  Degrade to the raw sweep and say so,
    # rather than refusing to look.
    records: list[NVSRecord] = []
    recognized_pages = 0
    pages_parsed = True
    page_error = ""
    try:
        records, recognized_pages = parse_nvs_records(dump, base)
    except ScanError as exc:
        pages_parsed = False
        page_error = str(exc)

    plaintext = scan_plaintext(dump, base, word_indexes)
    meta_keys, key_truncated = blob_candidates(records, "nkey")
    meta_blobs, blob_truncated = blob_candidates(records, "wblob")

    # Metadata first in both lists: dedupe_candidates keeps the first
    # provenance, so a blob found both ways is reported against its real record
    # and its real NVS state, and "raw" only appears where nothing else could
    # reach it.
    blobs = dedupe_candidates(meta_blobs + raw_blob_candidates(dump, base))
    stride = 1 if len(dump) <= RAW_FULL_SCAN_MAX else RAW_COARSE_STRIDE

    recoveries: dict[str, Recovery] = {}
    tried: set[bytes] = set()
    # No blob is the answer on its own: the key alone opens nothing, and the
    # sweep exists to find what a blob unlocks with.  This is the common case
    # after a REPLACE that worked, so it is the one worth not paying for.
    key_sources = (
        itertools.chain(
            dedupe_candidates(meta_keys), raw_key_candidates(dump, base, stride)
        )
        if blobs
        else iter(())
    )
    for key_candidate in key_sources:
        if key_candidate.data in tried:
            continue
        tried.add(key_candidate.data)
        mac_key = nvs_mac_subkey(key_candidate.data)
        for blob_candidate in blobs:
            opened = open_sealed(
                key_candidate.data, mac_key, blob_candidate.data, word_indexes
            )
            if opened is None:
                continue
            rid, word_count = opened
            recovery = recoveries.setdefault(
                rid, Recovery(rid, word_count, set(), set(), set())
            )
            recovery.key_addresses.update(r.address for r in key_candidate.records)
            recovery.blob_addresses.update(r.address for r in blob_candidate.records)
            recovery.states.update(r.state for r in key_candidate.records)
            recovery.states.update(r.state for r in blob_candidate.records)

    return ScanResult(
        size=len(dump),
        base=base,
        blank=all(byte == 0xFF for byte in dump),
        recognized_pages=recognized_pages,
        records=records,
        plaintext=plaintext,
        recoveries=recoveries,
        combinations_truncated=key_truncated or blob_truncated,
        pages_parsed=pages_parsed,
        page_error=page_error,
        raw_stride=stride,
    )


def _addresses(values: set[int]) -> str:
    return ", ".join(f"0x{value:X}" for value in sorted(values))


def render_result(result: ScanResult, what: str, label: str) -> str:
    lines = [
        f"{what} [{label}]  {result.size} bytes from 0x{result.base:X}"
        + ("  (all 0xFF: blank or never written)" if result.blank else "")
    ]
    if not result.pages_parsed:
        lines.append(
            f"  WARNING: NVS page geometry unreadable ({result.page_error}); "
            "record walk skipped, raw sweep only"
        )
    if not result.blank and result.pages_parsed and result.recognized_pages == 0:
        lines.append(
            "  WARNING: no plaintext NVS pages recognized; sealed-record result "
            "is inconclusive (encrypted or corrupt dump)"
        )
    if result.raw_stride != 1:
        lines.append(
            f"  note: raw sweep coarsened to {result.raw_stride}-byte alignment "
            "(dump larger than a partition); split it to sweep byte by byte"
        )

    if result.plaintext:
        lines.append("  readable BIP39 mnemonic residue:")
        for rid, (count, addresses) in sorted(result.plaintext.items()):
            lines.append(
                f"    recovery ID {rid}  {count} words  at {_addresses(addresses)}"
            )
    else:
        lines.append("  no BIP39 mnemonic readable as plaintext")

    if result.recoveries:
        lines.append("  recoverable sealed signer mnemonic:")
        for rid, recovery in sorted(result.recoveries.items()):
            states = ",".join(sorted(recovery.states))
            lines.append(
                f"    recovery ID {rid}  {recovery.word_count} words  "
                f"nkey@{_addresses(recovery.key_addresses)}  "
                f"wblob@{_addresses(recovery.blob_addresses)}  states={states}"
            )
    else:
        lines.append("  no recoverable sealed signer mnemonic found")

    distinct = set(result.plaintext) | set(result.recoveries)
    lines.append(f"  ({len(distinct)} distinct recovery ID{'s' if len(distinct) != 1 else ''})")
    if result.combinations_truncated:
        lines.append(
            "  WARNING: stale chunk combinations exceeded the safety bound; "
            "result may be incomplete"
        )
    return "\n".join(lines)


def default_wordlist() -> str:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(
        root,
        "components",
        "libwally-core",
        "upstream",
        "src",
        "data",
        "wordlists",
        "english.txt",
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="scan raw ESP-IDF NVS bytes for recoverable signer mnemonics"
    )
    parser.add_argument("dump", help="raw NVS or legacy flash-range dump")
    parser.add_argument("base", type=lambda value: int(value, 0))
    parser.add_argument("what", help="human-readable range label")
    parser.add_argument("label", nargs="?", default="dump")
    parser.add_argument("--wordlist", default=default_wordlist())
    args = parser.parse_args(argv[1:])

    try:
        with open(args.dump, "rb") as handle:
            dump = handle.read()
        _, word_indexes = load_wordlist(args.wordlist)
        result = scan_dump(dump, args.base, word_indexes)
    except (OSError, ScanError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 2
    print(render_result(result, args.what, args.label))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
