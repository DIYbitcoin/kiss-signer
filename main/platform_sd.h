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
// each group; dotfiles skipped). Returns count, <0 on error.
int  platform_sd_list_psbt(char names[][SD_NAME_LEN], int max);
int  platform_sd_read(const char *name, uint8_t *buf, size_t max, size_t *len);
int  platform_sd_write(const char *name, const uint8_t *buf, size_t len);
