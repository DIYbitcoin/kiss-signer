// SD card platform seam. Device: SDMMC slot 0 on the Guition JC4880P443C
// (CLK 43 / CMD 44 / D0-D3 39-42, IO MUX, card powered from on-chip LDO 4 —
// wiring taken from Guition's own board demo BSP). Sim: a host directory,
// so the whole Sign flow runs headless.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define SD_NAME_LEN 64

int  platform_sd_mount(void);      // 0 = mounted (idempotent)
void platform_sd_unmount(void);

// Hot-plug poll for the home screen. 1 = a card is present (and now mounted),
// 0 = absent. Cheap when a card is already mounted (just checks it's still
// there); attempts a mount when none is, so it also detects a fresh insert.
int  platform_sd_probe(void);

// Fill names[] with *.psbt files (unsigned first, signed second, A-Z within
// each group; dotfiles skipped). Returns count, <0 on error. When the card
// holds more than max, the FIRST max in that same order are kept -- never a
// directory-order arbitrary subset -- and *total (optional) carries the real
// count so the caller can say some are missing rather than showing a shorter
// card than the one in the slot.
int  platform_sd_list_psbt(char names[][SD_NAME_LEN], int max, int *total);
int  platform_sd_read(const char *name, uint8_t *buf, size_t max, size_t *len);
int  platform_sd_write(const char *name, const uint8_t *buf, size_t len);

// The device's own signed outputs (*-signed.psbt), in one directory pass.
//
// names/mark/n: mark[i] becomes 1 when names[i]'s "-signed.psbt" sibling is on
// the card. It reads the DIRECTORY rather than names[], and that is the whole
// point of the function. platform_sd_list_psbt keeps only the first `max` names
// in sort order and unsigned names sort first, so a card holding more unsigned
// files than the window has signed siblings the array never saw. Cross checking
// the array would answer "not signed" for files that are -- which is exactly the
// bug this exists to fix, reintroduced one layer down.
//
// del removes every signed output, via platform_sd_delete so interrupted-write
// sidecars go with it. Nothing that is not named *-signed.psbt is ever touched:
// an unsigned PSBT has no way to match the predicate.
//
// Returns how many signed outputs were found (removed, when del), <0 if the card
// could not be read. Requires an already mounted card.
int  platform_sd_signed_scan(char names[][SD_NAME_LEN], uint8_t *mark,
                             int n, int del);

// Just the signed outputs, A-Z, for the screen that removes them one at a time.
// Same reason it reads the directory rather than filtering the list: with more
// unsigned files than the list window holds, the signed ones are not in it.
int  platform_sd_list_signed(char names[][SD_NAME_LEN], int max, int *total);

// Firmware images, A-Z, same dotfile and window rules as list_psbt. The suffix
// is the ONLY thing checked here: a name carries no authority, so the gate on a
// firmware image is its header and its signature, not what it is called.
int  platform_sd_list_firmware(char names[][SD_NAME_LEN], int max, int *total);

// KEF backup envelopes (*.kef), same rules again. The name proves nothing:
// the file's bytes must parse as an envelope and the password must open it,
// so a mislabeled file simply fails the same vague way a corrupt one does.
int  platform_sd_list_kef(char names[][SD_NAME_LEN], int max, int *total);

// Streaming read, because a firmware image is megabytes and platform_sd_read
// wants the whole file in a caller buffer. Open reports the size up front so a
// caller can refuse an image too big for the slot before reading a byte of it.
//
// read_chunk returns 0 and sets *got, with *got == 0 meaning end of file. A
// short read is NOT an error: FAT over SDMMC returns what it has. Anything that
// is a real failure -- the card pulled mid read is the one that matters --
// comes back < 0, and the caller must treat a partial image as no image.
typedef struct platform_sd_file platform_sd_file;
platform_sd_file *platform_sd_open(const char *name, size_t *len);
int  platform_sd_read_chunk(platform_sd_file *f, uint8_t *buf, size_t max, size_t *got);
void platform_sd_close(platform_sd_file *f);

// Secret-bearing callers use the atomic form. It writes and verifies a sibling
// temporary file before switching names, keeping the previous file recoverable
// until the replacement is durable. delete also removes interrupted-write
// sidecars. Both require an already mounted/present card.
#define PLATFORM_SD_ATOMIC_CLEANUP 1   // target committed; stale sidecar remains
int  platform_sd_write_atomic(const char *name, const uint8_t *buf, size_t len);
int  platform_sd_delete(const char *name);       // absent file is success

#ifndef ESP_PLATFORM
// Native-test fault seam. The simulator never calls these, so its default is a
// present, working card. Flags are consumed by the next matching operation.
#define PLATFORM_SD_TEST_FAIL_READ    (1u << 0)
#define PLATFORM_SD_TEST_FAIL_WRITE   (1u << 1)
#define PLATFORM_SD_TEST_FAIL_RENAME  (1u << 2)
#define PLATFORM_SD_TEST_FAIL_DELETE  (1u << 3)
#define PLATFORM_SD_TEST_FAIL_BAK_DELETE (1u << 4)
void platform_sd_test_set_present(int present);
void platform_sd_test_fail_next(unsigned flags);
// Let n matching operations pass before the armed flag fires, so a fault can
// be aimed at the second write of a multi-file sequence.
void platform_sd_test_fail_skip(int n);
#endif
