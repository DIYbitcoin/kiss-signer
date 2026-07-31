/* Dependency-free C99 BitSquiggle32 core implementation.
 *
 * Grug 2-Clause License: do what want; not sue grug.
 */
#include "bitsquiggle32.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t count;
    uint8_t edge_class[BITSQUIGGLE32_EDGE_COUNT];
} ModeDefinition;

typedef struct {
    uint8_t first;
    uint8_t second;
} ClassKey;

/* Source-cell positions after removing primes from each copy template. */
static const uint8_t TEMPLATES[4][BITSQUIGGLE32_ROWS * BITSQUIGGLE32_COLUMNS] = {
    {0, 1, 2, 1, 0, 5, 6, 7, 6, 5, 10, 11, 12, 11, 10, 15, 16, 17, 16, 15,
     20, 21, 22, 21, 20, 25, 26, 27, 26, 25, 30, 31, 32, 31, 30},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
     10, 11, 12, 13, 14, 5, 6, 7, 8, 9, 0, 1, 2, 3, 4},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 16, 15,
     14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 8, 15, 16, 17, 12, 7,
     20, 21, 16, 11, 6, 25, 20, 15, 10, 5, 4, 3, 2, 1, 0}
};

static bool valid_style(Bitsquiggle32Style style) {
    return style >= BITSQUIGGLE32_STANDARD && style <= BITSQUIGGLE32_BLACK_AND_WHITE;
}

static bool valid_mode(Bitsquiggle32Mode mode) {
    return mode >= BITSQUIGGLE32_LEFT_RIGHT && mode <= BITSQUIGGLE32_DIAGONAL_SLASH;
}

void bitsquiggle32_edges(Bitsquiggle32Edge output[BITSQUIGGLE32_EDGE_COUNT]) {
    unsigned int row;
    unsigned int column;
    unsigned int index = 0;
    for (row = 0; row < BITSQUIGGLE32_ROWS; ++row) {
        for (column = 0; column < BITSQUIGGLE32_COLUMNS; ++column) {
            if (column + 1 < BITSQUIGGLE32_COLUMNS) {
                output[index].start_row = (uint8_t)row;
                output[index].start_column = (uint8_t)column;
                output[index].end_row = (uint8_t)row;
                output[index].end_column = (uint8_t)(column + 1);
                ++index;
            }
            if (row + 1 < BITSQUIGGLE32_ROWS) {
                output[index].start_row = (uint8_t)row;
                output[index].start_column = (uint8_t)column;
                output[index].end_row = (uint8_t)(row + 1);
                output[index].end_column = (uint8_t)column;
                ++index;
            }
        }
    }
}

static int compare_keys(ClassKey first, ClassKey second) {
    if (first.first != second.first) return first.first < second.first ? -1 : 1;
    if (first.second != second.second) return first.second < second.second ? -1 : 1;
    return 0;
}

static void make_definition(Bitsquiggle32Mode mode, ModeDefinition *definition) {
    Bitsquiggle32Edge edges[BITSQUIGGLE32_EDGE_COUNT];
    ClassKey edge_keys[BITSQUIGGLE32_EDGE_COUNT];
    ClassKey keys[BITSQUIGGLE32_EDGE_COUNT];
    unsigned int index;
    unsigned int count = 0;
    unsigned int other;
    unsigned int row;
    unsigned int column;

    bitsquiggle32_edges(edges);
    for (index = 0; index < BITSQUIGGLE32_EDGE_COUNT; ++index) {
        uint8_t first = TEMPLATES[mode][edges[index].start_row * BITSQUIGGLE32_COLUMNS
                                        + edges[index].start_column];
        uint8_t second = TEMPLATES[mode][edges[index].end_row * BITSQUIGGLE32_COLUMNS
                                         + edges[index].end_column];
        edge_keys[index].first = first < second ? first : second;
        edge_keys[index].second = first < second ? second : first;
        for (other = 0; other < count; ++other) {
            if (compare_keys(keys[other], edge_keys[index]) == 0) break;
        }
        if (other == count) keys[count++] = edge_keys[index];
    }
    for (row = 0; row < count; ++row) {
        for (column = row + 1; column < count; ++column) {
            if (compare_keys(keys[column], keys[row]) < 0) {
                ClassKey temporary = keys[row];
                keys[row] = keys[column];
                keys[column] = temporary;
            }
        }
    }
    definition->count = (uint8_t)count;
    for (index = 0; index < BITSQUIGGLE32_EDGE_COUNT; ++index) {
        for (other = 0; other < count; ++other) {
            if (compare_keys(keys[other], edge_keys[index]) == 0) break;
        }
        definition->edge_class[index] = (uint8_t)other;
    }
}

uint32_t bitsquiggle32_mix32(uint32_t value) {
    value += UINT32_C(0x9e3779b9);
    value = (value ^ (value >> 16)) * UINT32_C(0x85ebca6b);
    value = (value ^ (value >> 13)) * UINT32_C(0xc2b2ae35);
    return value ^ (value >> 16);
}

unsigned int bitsquiggle32_free_connection_count(Bitsquiggle32Mode mode) {
    ModeDefinition definition;
    if (!valid_mode(mode)) return 0;
    make_definition(mode, &definition);
    return definition.count;
}

bool bitsquiggle32_matches_mode(const uint8_t connections[BITSQUIGGLE32_EDGE_COUNT],
                                Bitsquiggle32Mode mode) {
    ModeDefinition definition;
    uint8_t values[BITSQUIGGLE32_EDGE_COUNT];
    unsigned int index;
    if (connections == NULL || !valid_mode(mode)) return false;
    make_definition(mode, &definition);
    memset(values, 2, sizeof(values));
    for (index = 0; index < BITSQUIGGLE32_EDGE_COUNT; ++index) {
        uint8_t class_index = definition.edge_class[index];
        if (values[class_index] == 2) values[class_index] = connections[index];
        else if (values[class_index] != connections[index]) return false;
    }
    return true;
}

const char *bitsquiggle32_mode_label(Bitsquiggle32Mode mode) {
    static const char *const labels[] = {"A|", "A-", "A+", "A/"};
    return valid_mode(mode) ? labels[mode] : NULL;
}

static void expand(const ModeDefinition *definition, const uint8_t *values,
                   uint8_t connections[BITSQUIGGLE32_EDGE_COUNT]) {
    unsigned int index;
    for (index = 0; index < BITSQUIGGLE32_EDGE_COUNT; ++index) {
        connections[index] = values[definition->edge_class[index]];
    }
}

static void encode_default(uint32_t mixed,
                           uint8_t connections[BITSQUIGGLE32_EDGE_COUNT]) {
    ModeDefinition definition;
    uint8_t values[32];
    unsigned int index;
    make_definition(BITSQUIGGLE32_LEFT_RIGHT, &definition);
    for (index = 0; index < 32; ++index) values[index] = (uint8_t)(mixed >> (31 - index)) & 1u;
    expand(&definition, values, connections);
}

static void encode_connections(uint32_t mixed, Bitsquiggle32Mode mode,
                               uint8_t connections[BITSQUIGGLE32_EDGE_COUNT]) {
    ModeDefinition definition;
    uint8_t values[BITSQUIGGLE32_EDGE_COUNT];
    uint32_t payload = mixed & UINT32_C(0x3fffffff);
    unsigned int index;
    if (mode == BITSQUIGGLE32_LEFT_RIGHT) {
        encode_default(mixed, connections);
        return;
    }
    make_definition(mode, &definition);
    for (index = 0; index < definition.count; ++index) {
        values[index] = (uint8_t)(payload >> (29 - (index % 30))) & 1u;
    }
    expand(&definition, values, connections);
}

static bool preferred_mode_has_capacity(uint32_t mixed, Bitsquiggle32Mode mode) {
    ModeDefinition definition;
    unsigned int missing_bits;
    make_definition(mode, &definition);
    if (definition.count >= 30u) return true;
    missing_bits = 30u - definition.count;
    return (mixed & ((UINT32_C(1) << missing_bits) - 1u)) == 0;
}

static bool conflicts_with_earlier_mode(const uint8_t connections[BITSQUIGGLE32_EDGE_COUNT],
                                        Bitsquiggle32Mode mode) {
    unsigned int earlier;
    for (earlier = 0; earlier < (unsigned int)mode; ++earlier) {
        if (bitsquiggle32_matches_mode(connections, (Bitsquiggle32Mode)earlier)) return true;
    }
    return false;
}

static void active_cells(const uint8_t connections[BITSQUIGGLE32_EDGE_COUNT],
                         uint8_t cells[BITSQUIGGLE32_ROWS][BITSQUIGGLE32_COLUMNS]) {
    Bitsquiggle32Edge edges[BITSQUIGGLE32_EDGE_COUNT];
    unsigned int index;
    memset(cells, 0, BITSQUIGGLE32_ROWS * BITSQUIGGLE32_COLUMNS);
    bitsquiggle32_edges(edges);
    for (index = 0; index < BITSQUIGGLE32_EDGE_COUNT; ++index) {
        if (connections[index] == 0) continue;
        cells[edges[index].start_row][edges[index].start_column] = 1;
        cells[edges[index].end_row][edges[index].end_column] = 1;
    }
}

static double clamp01(double value) {
    if (value < 0.0) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
}

static double srgb_encode(double value) {
    value = clamp01(value);
    return value <= 0.0031308 ? 12.92 * value : 1.055 * pow(value, 1.0 / 2.4) - 0.055;
}

static void make_color(double lightness, double chroma, double hue,
                       Bitsquiggle32Color *color) {
    double radians = hue * acos(-1.0) / 180.0;
    double a = chroma * cos(radians);
    double b = chroma * sin(radians);
    double l_value = lightness + 0.3963377774 * a + 0.2158037573 * b;
    double m_value = lightness - 0.1055613458 * a - 0.0638541728 * b;
    double s_value = lightness - 0.0894841775 * a - 1.2914855480 * b;
    double l3 = l_value * l_value * l_value;
    double m3 = m_value * m_value * m_value;
    double s3 = s_value * s_value * s_value;
    int red = (int)(srgb_encode(4.0767416621 * l3 - 3.3077115913 * m3
                                 + 0.2309699292 * s3) * 255.0 + 0.5);
    int green = (int)(srgb_encode(-1.2684380046 * l3 + 2.6097574011 * m3
                                   - 0.3413193965 * s3) * 255.0 + 0.5);
    int blue = (int)(srgb_encode(-0.0041960863 * l3 - 0.7034186147 * m3
                                  + 1.7076147010 * s3) * 255.0 + 0.5);
    color->lightness = lightness;
    color->chroma = chroma;
    color->hue = hue;
    (void)snprintf(color->hex, sizeof(color->hex), "#%02x%02x%02x", red, green, blue);
}

static unsigned int popcount32(uint32_t value) {
    unsigned int count = 0;
    while (value != 0) {
        value &= value - 1;
        ++count;
    }
    return count;
}

static void derive_colors(uint8_t hue_index, uint8_t chroma_index,
                          uint8_t luminance_index, bool swapped,
                          Bitsquiggle32Style style, Bitsquiggle32Color *background,
                          Bitsquiggle32Color *foreground) {
    double foreground_hue = hue_index * (360.0 / 16.0);
    double background_hue = fmod(foreground_hue + 180.0, 360.0);
    double chroma = 0.05 + chroma_index * ((0.25 - 0.05) / 15.0);
    double base_lightness = 0.50 + luminance_index * ((0.70 - 0.50) / 3.0);
    double foreground_lightness;
    double background_lightness;
    double foreground_chroma;
    double background_chroma;
    if (style == BITSQUIGGLE32_STANDARD) {
        foreground_lightness = base_lightness;
        background_lightness = foreground_lightness - 0.50;
        foreground_chroma = chroma;
        background_chroma = chroma;
    } else {
        foreground_lightness = (style == BITSQUIGGLE32_BLACK_AND_WHITE) ? 1.0 : base_lightness + 0.30;
        background_lightness = (style == BITSQUIGGLE32_BLACK_AND_WHITE) ? 0.0 : foreground_lightness - 0.80;
        foreground_chroma = (style == BITSQUIGGLE32_MONOCHROME
                              || style == BITSQUIGGLE32_BLACK_AND_WHITE)
            ? 0.0 : chroma + 0.10;
        background_chroma = foreground_chroma;
    }
    foreground_lightness = clamp01(foreground_lightness);
    background_lightness = clamp01(background_lightness);
    if (swapped) {
        double temporary = foreground_lightness;
        foreground_lightness = background_lightness;
        background_lightness = temporary;
    }
    make_color(background_lightness, background_chroma, background_hue, background);
    make_color(foreground_lightness, foreground_chroma, foreground_hue, foreground);
}

int bitsquiggle32_spec(uint32_t input, Bitsquiggle32Style style,
                       Bitsquiggle32Spec *output) {
    uint32_t mixed;
    Bitsquiggle32Mode preferred;
    uint8_t candidate[BITSQUIGGLE32_EDGE_COUNT];
    bool fallback;
    if (output == NULL || !valid_style(style)) return -1;
    mixed = bitsquiggle32_mix32(input);
    preferred = (Bitsquiggle32Mode)(mixed >> 30);
    encode_connections(mixed, preferred, candidate);
    fallback = preferred != BITSQUIGGLE32_LEFT_RIGHT
        && (!preferred_mode_has_capacity(mixed, preferred)
            || conflicts_with_earlier_mode(candidate, preferred));
    output->input = input;
    output->mixed = mixed;
    output->style = style;
    output->preferred_mode = preferred;
    output->actual_mode = fallback ? BITSQUIGGLE32_LEFT_RIGHT : preferred;
    output->fallback = fallback;
    if (fallback) encode_default(mixed, output->connections);
    else memcpy(output->connections, candidate, sizeof(candidate));
    active_cells(output->connections, output->cells);
    output->luminance_index = (uint8_t)(mixed & 0x3u);
    output->swapped = (popcount32(input) & 1u) != 0;
    derive_colors((uint8_t)((mixed >> 12) & 0xfu), (uint8_t)((mixed >> 8) & 0xfu),
                  output->luminance_index, output->swapped, style,
                  &output->background, &output->foreground);
    return 0;
}

static uint8_t connection_at(const uint8_t connections[BITSQUIGGLE32_EDGE_COUNT],
                             uint8_t start_row, uint8_t start_column,
                             uint8_t end_row, uint8_t end_column) {
    Bitsquiggle32Edge edges[BITSQUIGGLE32_EDGE_COUNT];
    unsigned int index;
    bitsquiggle32_edges(edges);
    for (index = 0; index < BITSQUIGGLE32_EDGE_COUNT; ++index) {
        if (edges[index].start_row == start_row && edges[index].start_column == start_column
            && edges[index].end_row == end_row && edges[index].end_column == end_column) {
            return connections[index];
        }
    }
    return 0;
}

int bitsquiggle32_pixels(uint32_t input, Bitsquiggle32Style style,
                         Bitsquiggle32PixelGrid *output) {
    Bitsquiggle32Spec visual;
    Bitsquiggle32Edge edges[BITSQUIGGLE32_EDGE_COUNT];
    unsigned int index;
    unsigned int row;
    unsigned int column;
    if (output == NULL || bitsquiggle32_spec(input, style, &visual) != 0) return -1;
    output->width = BITSQUIGGLE32_PIXEL_WIDTH;
    output->height = BITSQUIGGLE32_PIXEL_HEIGHT;
    output->background = visual.background;
    output->foreground = visual.foreground;
    output->style = style;
    memset(output->pixels, 0, sizeof(output->pixels));
    for (row = 0; row < BITSQUIGGLE32_ROWS; ++row) {
        for (column = 0; column < BITSQUIGGLE32_COLUMNS; ++column) {
            unsigned int x;
            unsigned int y;
            if (visual.cells[row][column] == 0) continue;
            x = 1 + 3 * column;
            y = 1 + 3 * row;
            output->pixels[y * BITSQUIGGLE32_PIXEL_WIDTH + x] = 1;
            output->pixels[y * BITSQUIGGLE32_PIXEL_WIDTH + x + 1] = 1;
            output->pixels[(y + 1) * BITSQUIGGLE32_PIXEL_WIDTH + x] = 1;
            output->pixels[(y + 1) * BITSQUIGGLE32_PIXEL_WIDTH + x + 1] = 1;
        }
    }
    bitsquiggle32_edges(edges);
    for (index = 0; index < BITSQUIGGLE32_EDGE_COUNT; ++index) {
        unsigned int x;
        unsigned int y;
        if (visual.connections[index] == 0) continue;
        x = 1 + 3 * edges[index].start_column;
        y = 1 + 3 * edges[index].start_row;
        if (edges[index].start_row == edges[index].end_row) {
            output->pixels[y * BITSQUIGGLE32_PIXEL_WIDTH + x + 2] = 1;
            output->pixels[(y + 1) * BITSQUIGGLE32_PIXEL_WIDTH + x + 2] = 1;
        } else {
            output->pixels[(y + 2) * BITSQUIGGLE32_PIXEL_WIDTH + x] = 1;
            output->pixels[(y + 2) * BITSQUIGGLE32_PIXEL_WIDTH + x + 1] = 1;
        }
    }
    for (row = 0; row + 1 < BITSQUIGGLE32_ROWS; ++row) {
        for (column = 0; column + 1 < BITSQUIGGLE32_COLUMNS; ++column) {
            if (connection_at(visual.connections, (uint8_t)row, (uint8_t)column,
                              (uint8_t)row, (uint8_t)(column + 1))
                && connection_at(visual.connections, (uint8_t)(row + 1), (uint8_t)column,
                                 (uint8_t)(row + 1), (uint8_t)(column + 1))
                && connection_at(visual.connections, (uint8_t)row, (uint8_t)column,
                                 (uint8_t)(row + 1), (uint8_t)column)
                && connection_at(visual.connections, (uint8_t)row, (uint8_t)(column + 1),
                                 (uint8_t)(row + 1), (uint8_t)(column + 1))) {
                output->pixels[(3 + 3 * row) * BITSQUIGGLE32_PIXEL_WIDTH + 3 + 3 * column] = 1;
            }
        }
    }
    return 0;
}

static uint64_t horizontal_edge_bit(unsigned int row, unsigned int column) {
    unsigned int offset = row == BITSQUIGGLE32_ROWS - 1u
        ? row * (2u * BITSQUIGGLE32_COLUMNS - 1u) + column
        : row * (2u * BITSQUIGGLE32_COLUMNS - 1u) + 2u * column;
    return UINT64_C(1) << offset;
}

static uint64_t vertical_edge_bit(unsigned int row, unsigned int column) {
    unsigned int offset = column == BITSQUIGGLE32_COLUMNS - 1u
        ? 2u * BITSQUIGGLE32_COLUMNS - 2u : 2u * column + 1u;
    return UINT64_C(1) << (row * (2u * BITSQUIGGLE32_COLUMNS - 1u) + offset);
}

static uint32_t junction_bit(unsigned int row, unsigned int column) {
    return UINT32_C(1) << (row * (BITSQUIGGLE32_COLUMNS - 1u) + column);
}

static bool required_edge_mask(const uint8_t connections[BITSQUIGGLE32_EDGE_COUNT],
                               uint64_t *output) {
    unsigned int index;
    uint64_t result = 0;
    if (connections == NULL) return false;
    for (index = 0; index < BITSQUIGGLE32_EDGE_COUNT; ++index) {
        if (connections[index] != 0u && connections[index] != 1u) return false;
        if (connections[index] != 0u) result |= UINT64_C(1) << index;
    }
    *output = result;
    return true;
}

static uint64_t connected_rectangle_edge_mask(unsigned int top, unsigned int left,
                                              unsigned int bottom, unsigned int right,
                                              uint64_t required_edges) {
    unsigned int row;
    unsigned int column;
    uint64_t result = 0;
    for (row = top; row <= bottom; ++row) {
        for (column = left; column <= right; ++column) {
            if (column < right) {
                uint64_t edge = horizontal_edge_bit(row, column);
                if ((required_edges & edge) == 0) return 0;
                result |= edge;
            }
            if (row < bottom) {
                uint64_t edge = vertical_edge_bit(row, column);
                if ((required_edges & edge) == 0) return 0;
                result |= edge;
            }
        }
    }
    return result;
}

static uint32_t required_junction_mask(uint64_t required_edges) {
    unsigned int row;
    unsigned int column;
    uint32_t result = 0;
    for (row = 0; row + 1u < BITSQUIGGLE32_ROWS; ++row) {
        for (column = 0; column + 1u < BITSQUIGGLE32_COLUMNS; ++column) {
            if (connected_rectangle_edge_mask(row, column, row + 1u, column + 1u,
                                              required_edges) != 0) {
                result |= junction_bit(row, column);
            }
        }
    }
    return result;
}

static uint32_t rectangle_junction_mask(unsigned int top, unsigned int left,
                                        unsigned int bottom, unsigned int right,
                                        uint32_t required_junctions) {
    unsigned int row;
    unsigned int column;
    uint32_t result = 0;
    for (row = top; row < bottom; ++row) {
        for (column = left; column < right; ++column) {
            uint32_t junction = junction_bit(row, column);
            if ((required_junctions & junction) != 0) result |= junction;
        }
    }
    return result;
}

static unsigned int popcount64(uint64_t value) {
    unsigned int count = 0;
    while (value != 0) {
        value &= value - 1;
        ++count;
    }
    return count;
}

static bool is_better_blob(unsigned int top, unsigned int left,
                           unsigned int bottom, unsigned int right,
                           uint64_t new_edges, uint32_t new_junctions,
                           unsigned int best_top, unsigned int best_left,
                           unsigned int best_bottom, unsigned int best_right,
                           uint64_t best_new_edges, uint32_t best_new_junctions) {
    unsigned int junction_count = popcount32(new_junctions);
    unsigned int best_junction_count = popcount32(best_new_junctions);
    unsigned int edge_count;
    unsigned int best_edge_count;
    unsigned int area;
    unsigned int best_area;
    if (junction_count != best_junction_count) return junction_count > best_junction_count;
    edge_count = popcount64(new_edges);
    best_edge_count = popcount64(best_new_edges);
    if (edge_count != best_edge_count) return edge_count > best_edge_count;
    area = (bottom - top + 1u) * (right - left + 1u);
    best_area = (best_bottom - best_top + 1u) * (best_right - best_left + 1u);
    if (area != best_area) return area < best_area;
    if (top != best_top) return top < best_top;
    if (left != best_left) return left < best_left;
    if (bottom != best_bottom) return bottom < best_bottom;
    return right < best_right;
}

static int first_uncovered_edge(uint64_t required_edges, uint64_t covered_edges) {
    unsigned int index;
    uint64_t remaining = required_edges & ~covered_edges;
    for (index = 0; index < BITSQUIGGLE32_EDGE_COUNT; ++index) {
        if ((remaining & (UINT64_C(1) << index)) != 0) return (int)index;
    }
    return -1;
}

static int first_uncovered_junction(uint32_t required_junctions,
                                    uint32_t covered_junctions) {
    unsigned int index;
    uint32_t remaining = required_junctions & ~covered_junctions;
    for (index = 0; index < (BITSQUIGGLE32_ROWS - 1u) * (BITSQUIGGLE32_COLUMNS - 1u); ++index) {
        if ((remaining & (UINT32_C(1) << index)) != 0) return (int)index;
    }
    return -1;
}

int bitsquiggle32_smooth_blobs(
    const uint8_t connections[BITSQUIGGLE32_EDGE_COUNT],
    Bitsquiggle32SmoothBlob output[BITSQUIGGLE32_MAX_SMOOTH_BLOBS]) {
    Bitsquiggle32Edge edges[BITSQUIGGLE32_EDGE_COUNT];
    uint64_t required_edges;
    uint32_t required_junctions;
    uint64_t covered_edges = 0;
    uint32_t covered_junctions = 0;
    unsigned int count = 0;
    if (output == NULL || !required_edge_mask(connections, &required_edges)) return -1;
    required_junctions = required_junction_mask(required_edges);
    bitsquiggle32_edges(edges);

    while (covered_edges != required_edges || covered_junctions != required_junctions) {
        int anchor_edge = first_uncovered_edge(required_edges, covered_edges);
        int anchor_junction = anchor_edge < 0
            ? first_uncovered_junction(required_junctions, covered_junctions) : -1;
        unsigned int anchor_top;
        unsigned int anchor_left;
        unsigned int anchor_bottom;
        unsigned int anchor_right;
        unsigned int best_top = 0;
        unsigned int best_left = 0;
        unsigned int best_bottom = 0;
        unsigned int best_right = 0;
        uint64_t best_edges = 0;
        uint32_t best_junctions = 0;
        unsigned int left_inclusive = 0;
        bool found = false;
        int top;
        int left;

        if (anchor_edge >= 0) {
            Bitsquiggle32Edge edge = edges[anchor_edge];
            anchor_top = edge.start_row;
            anchor_left = edge.start_column;
            anchor_bottom = edge.end_row;
            anchor_right = edge.end_column;
        } else {
            anchor_top = (unsigned int)anchor_junction / (BITSQUIGGLE32_COLUMNS - 1u);
            anchor_left = (unsigned int)anchor_junction % (BITSQUIGGLE32_COLUMNS - 1u);
            anchor_bottom = anchor_top + 1u;
            anchor_right = anchor_left + 1u;
        }

        for (top = (int)anchor_top; top >= 0; --top) {
            for (left = (int)anchor_left; left >= (int)left_inclusive; --left) {
                unsigned int right_exclusive = BITSQUIGGLE32_COLUMNS;
                bool stop_left_expansion = false;
                unsigned int bottom;
                for (bottom = anchor_bottom; bottom < BITSQUIGGLE32_ROWS; ++bottom) {
                    unsigned int right;
                    for (right = anchor_right; right < right_exclusive; ++right) {
                        uint64_t candidate_edges = connected_rectangle_edge_mask(
                            (unsigned int)top, (unsigned int)left, bottom, right, required_edges);
                        uint32_t candidate_junctions;
                        uint64_t new_edges;
                        uint32_t new_junctions;
                        if (candidate_edges == 0) {
                            if (bottom == anchor_bottom && right == anchor_right) {
                                left_inclusive = (unsigned int)left + 1u;
                                stop_left_expansion = true;
                            } else {
                                right_exclusive = right;
                            }
                            break;
                        }
                        candidate_junctions = rectangle_junction_mask(
                            (unsigned int)top, (unsigned int)left, bottom, right,
                            required_junctions);
                        new_edges = candidate_edges & ~covered_edges;
                        new_junctions = candidate_junctions & ~covered_junctions;
                        if (new_edges == 0 && new_junctions == 0) continue;
                        if (!found || is_better_blob(
                                (unsigned int)top, (unsigned int)left, bottom, right,
                                new_edges, new_junctions, best_top, best_left,
                                best_bottom, best_right, best_edges & ~covered_edges,
                                best_junctions & ~covered_junctions)) {
                            best_top = (unsigned int)top;
                            best_left = (unsigned int)left;
                            best_bottom = bottom;
                            best_right = right;
                            best_edges = candidate_edges;
                            best_junctions = candidate_junctions;
                            found = true;
                        }
                    }
                    if (stop_left_expansion || right_exclusive == anchor_right) break;
                }
                if (stop_left_expansion) break;
            }
        }
        if (!found || count == BITSQUIGGLE32_MAX_SMOOTH_BLOBS) return -1;
        output[count].top_row = (uint8_t)best_top;
        output[count].left_column = (uint8_t)best_left;
        output[count].bottom_row = (uint8_t)best_bottom;
        output[count].right_column = (uint8_t)best_right;
        ++count;
        covered_edges |= best_edges;
        covered_junctions |= best_junctions;
    }
    return (int)count;
}
