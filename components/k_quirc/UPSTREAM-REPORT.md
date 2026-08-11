# Two fixes for odudex/k_quirc

Found auditing the copy vendored here at `06549efae32a4378216b868b2fc2e93cbcdd9707`.
Both are still present at upstream HEAD (checked 2026-08-11, last commit 2026-07-29),
so they were inherited, not introduced by our vendoring.

**Send as a pull request, not a security report.** Krux's SECURITY.md asks for
vulnerabilities "that could potentially affect the security of users' funds", and
neither of these does. The out of bounds access is a READ of read only data that
yields garbage characters in a decoded string: no write, no corruption, no key
material, and not reachable from a conforming QR at all. The version table entry is
plain incorrectness. Neither is exploitable for funds.

Nor is this shipping Krux: `selfcustody/krux` only names k_quirc in its CHANGELOG.
The submodule is pinned by `odudex/kern`, odudex's ESP32 firmware.

Neither came from Daniel Beer's quirc or from OpenMV — both of those are correct.
They were introduced when `alpha_tuple()` was inlined into `decode_alpha()`.

---

## 1. Out of bounds read in `decode_alpha` (src/k_quirc_decode.c)

`alpha_map` is 45 characters plus a NUL, 46 bytes. Two indexes into it are wider:

```c
d = take_bits(ds, 11);
data->payload[data->payload_len++] = alpha_map[d / 45];   /* d/45 reaches 45  */
data->payload[data->payload_len++] = alpha_map[d % 45];   /* fine             */
...
d = take_bits(ds, 6);
data->payload[data->payload_len++] = alpha_map[d];        /* d reaches 63     */
```

* The 6 bit path indexes 0..63, so **46..63 read up to 17 bytes past the literal**
  and copy whatever `.rodata` follows into the decoded payload.
* The 11 bit path can produce `d/45 == 45`, which reads the NUL and writes a zero
  byte into the middle of a payload that callers commonly treat as a C string.

Not reachable from a conforming symbol — a real encoder never emits a value outside
the alphabet — but this is the first code to touch bytes off a QR code held up to a
camera, and nothing upstream of it masks the value.

Both of quirc's ancestors avoid this by construction, because `alpha_tuple()` applies
`% 45` to **every** index:

```c
/* dlbeer/quirc and openmv, both identical */
data->payload[data->payload_len + digits - i - 1] = alpha_map[tuple % 45];
tuple /= 45;
```

### Suggested fix

Restoring `% 45` would fix the memory safety and match the ancestors. We chose to
**reject** instead, because on a signer silently folding an impossible value into a
valid character hands the owner a plausible looking wrong string:

```c
int k_quirc_alpha_char(int v) {
  return (v >= 0 && v < 45) ? (int)(unsigned char)alpha_map[v] : -1;
}
```

and both call sites return `K_QUIRC_ERROR_DATA_ECC` on `-1`. A value outside the
alphabet means the corrected data is not alphanumeric, whatever the mode bits said.
Either fix closes the read; the choice is about what a malformed symbol should do.

---

## 2. Version 25-Q can never decode (src/k_quirc_version.c)

`read_data()` derives the long block count from `data_bytes`:

```c
lb_count = (ver->data_bytes - sb_ecc->bs * sb_ecc->ns) / (sb_ecc->bs + 1);
bc       = lb_count + sb_ecc->ns;
```

so every row of `quirc_version_db` must satisfy `ns*bs + lb*(bs+1) == data_bytes`
exactly, with `lb` a whole number. Version 25, ECC level Q ships `ns = 3`:

    3*54 + lb*55 == 1588  ->  lb = 25.93...    (not whole)

`ns = 7` is the only value in range that closes it:

    7*54 + 22*55 == 1588                        (exact)

Checking all 100 rows against that invariant finds exactly this one. The invariant is
the decoder's own arithmetic, so the check needs no copy of the ISO 18004 tables:

```c
for (int v = 1; v <= QUIRC_MAX_VERSION; v++)
  for (int e = 0; e < 4; e++) {
    const struct quirc_rs_params *p = &quirc_version_db[v].ecc[e];
    int rest = quirc_version_db[v].data_bytes - p->bs * p->ns;
    assert(rest >= 0 && rest % (p->bs + 1) == 0);
  }
```

Worth keeping as a test — it is cheap and it catches the whole class.

---

## Also worth a look (not reported as defects)

* `find_alignment_pattern()` bounds its spiral by `size_estimate * 100`, where
  `size_estimate` is a cross product of perspective mapped capstone corners, so a
  crafted image can make it very large. There is no yield inside that loop. On
  FreeRTOS that is a task watchdog reset rather than a wrong answer. We capped the
  walk at the image diagonal, past which the search is off frame anyway.
* `threshold()` declares four 256 entry `uint32_t` histograms as automatics, 4096
  bytes in one frame. Fine on a generous stack; ours is a 6 KB camera task, so we
  made them static (single threaded decode, zeroed per call).
