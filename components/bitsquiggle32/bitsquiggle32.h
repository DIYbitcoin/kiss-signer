/* Dependency-free C99 BitSquiggle32 core API.
 *
 * Grug 2-Clause License: do what want; not sue grug.
 */
#ifndef BITSQUIGGLE32_H
#define BITSQUIGGLE32_H

#include <stdbool.h>
#include <stdint.h>

#define BITSQUIGGLE32_ROWS 7u
#define BITSQUIGGLE32_COLUMNS 5u
#define BITSQUIGGLE32_EDGE_COUNT 58u
#define BITSQUIGGLE32_PIXEL_WIDTH 16u
#define BITSQUIGGLE32_PIXEL_HEIGHT 22u
#define BITSQUIGGLE32_MAX_SMOOTH_BLOBS 82u

typedef enum {
    BITSQUIGGLE32_STANDARD,
    BITSQUIGGLE32_HIGH_CONTRAST,
    BITSQUIGGLE32_MONOCHROME,
    BITSQUIGGLE32_BLACK_AND_WHITE
} Bitsquiggle32Style;

typedef enum {
    BITSQUIGGLE32_LEFT_RIGHT,
    BITSQUIGGLE32_TOP_BOTTOM,
    BITSQUIGGLE32_HALF_TURN,
    BITSQUIGGLE32_DIAGONAL_SLASH
} Bitsquiggle32Mode;

typedef struct {
    uint8_t start_row;
    uint8_t start_column;
    uint8_t end_row;
    uint8_t end_column;
} Bitsquiggle32Edge;

typedef struct {
    double lightness;
    double chroma;
    double hue;
    char hex[8];
} Bitsquiggle32Color;

typedef struct {
    uint32_t input;
    uint32_t mixed;
    uint8_t connections[BITSQUIGGLE32_EDGE_COUNT];
    uint8_t cells[BITSQUIGGLE32_ROWS][BITSQUIGGLE32_COLUMNS];
    Bitsquiggle32Color background;
    Bitsquiggle32Color foreground;
    Bitsquiggle32Style style;
    Bitsquiggle32Mode preferred_mode;
    Bitsquiggle32Mode actual_mode;
    bool fallback;
    uint8_t luminance_index;
    bool swapped;
} Bitsquiggle32Spec;

typedef struct {
    uint8_t width;
    uint8_t height;
    uint8_t pixels[BITSQUIGGLE32_PIXEL_WIDTH * BITSQUIGGLE32_PIXEL_HEIGHT];
    Bitsquiggle32Color background;
    Bitsquiggle32Color foreground;
    Bitsquiggle32Style style;
} Bitsquiggle32PixelGrid;

typedef struct {
    uint8_t top_row;
    uint8_t left_column;
    uint8_t bottom_row;
    uint8_t right_column;
} Bitsquiggle32SmoothBlob;

/* Return zero on success and -1 for an invalid style or null output pointer. */
int bitsquiggle32_spec(uint32_t input, Bitsquiggle32Style style,
                       Bitsquiggle32Spec *output);
int bitsquiggle32_pixels(uint32_t input, Bitsquiggle32Style style,
                         Bitsquiggle32PixelGrid *output);

/* Return the number of ordered smooth blobs, or -1 for invalid input/output.
 * The caller supplies storage for BITSQUIGGLE32_MAX_SMOOTH_BLOBS entries. */
int bitsquiggle32_smooth_blobs(const uint8_t connections[BITSQUIGGLE32_EDGE_COUNT],
                               Bitsquiggle32SmoothBlob output[BITSQUIGGLE32_MAX_SMOOTH_BLOBS]);

uint32_t bitsquiggle32_mix32(uint32_t value);
unsigned int bitsquiggle32_free_connection_count(Bitsquiggle32Mode mode);
bool bitsquiggle32_matches_mode(const uint8_t connections[BITSQUIGGLE32_EDGE_COUNT],
                                Bitsquiggle32Mode mode);
void bitsquiggle32_edges(Bitsquiggle32Edge output[BITSQUIGGLE32_EDGE_COUNT]);
const char *bitsquiggle32_mode_label(Bitsquiggle32Mode mode);

#endif
