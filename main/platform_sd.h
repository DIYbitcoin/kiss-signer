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
#endif
