# slhdsa-c, vendored

`upstream/` is https://github.com/pq-code-package/slhdsa-c at

    174c02e42257f95c210963272877c49dbb50070f   Thu Aug 6 2026

FIPS 205 SLH-DSA, portable C, no malloc anywhere and no dependencies. The code
began as SLotH driver code by Markku-Juhani O. Saarinen and was donated to the
project; the licence is `Apache-2.0 OR ISC OR MIT`, and this repository takes it
under MIT. `upstream/LICENSE` is the file as shipped.

## The one line that is not upstream

`sha2_256.c`, the definition of `sha2_256_compress`, now reads

```c
#ifdef PQ_SHA256_COMPRESS_HOOK
void sha2_256_compress_sw(void *v)
#else
void sha2_256_compress(void *v)
#endif
```

and nothing else in the tree is touched. Undefine the macro and the file is
upstream again, byte for byte.

It has to be that symbol and it has to be the definition. Every SHA-256 in
SLH-DSA arrives through `sha2_256_update()` or `sha2_256_final_len()`, both of
which call `sha2_256_compress()` from inside this same file, so it is the only
seam a hardware SHA can be dropped into -- there is no higher one, and the
parameter sets reach it through function pointers that all land here. A macro
rename applied at the compiler (`-Dsha2_256_compress=...`) rewrites those call
sites along with the definition, which sends every hash back to software and
leaves the accelerator connected to nothing. That looks like it works, produces
correct digests, and is four times slower.

`pq_hw_sha.c` supplies `sha2_256_compress` under the old name and decides per
call whether the peripheral is held.

## Re-vendoring

    git clone --depth 1 https://github.com/pq-code-package/slhdsa-c
    cp slhdsa-c/*.c slhdsa-c/*.h slhdsa-c/LICENSE components/slhdsa/upstream/

then re-apply the block above, and run `sim/build_test.sh` -- `sim/test_pq.c`
holds the FIPS 205 vectors and will say so if the drop moved.

## What is compiled, and what is not

`CMakeLists.txt` builds the SHA2 half only: SLH-DSA-SHA2-128s is the parameter
set this firmware uses and the SHAKE sets reach it through nothing. The SHAKE
and SHA3 sources stay in the tree so the drop matches upstream and the next
re-vendor is a copy, but shipping crypto no test on this project ever runs is
worse than shipping less of it.
