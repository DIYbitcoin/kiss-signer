// Native LVGL QR regression: production widget bitmap -> production camera
// decoder -> cUR assembly. Synthetic bytes exercise transport, not signing.
// Optional --file <PSBT> adds an externally verified payload; --dump <dir>
// writes actual card rasters and payload text for an independent image reader.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include "lvgl.h"
#include "kiss_theme.h"
#include "qr_transport.h"
#include "k_quirc.h"
#include "ur_decoder.h"
#include "ur_encoder.h"
#include "types/psbt.h"

static unsigned checks;
static const char *dump_dir;
static void check(bool ok, const char *what)
{
    ++checks;
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *p)
{ (void)a; (void)p; lv_display_flush_ready(d); }
static void service(void)
{ lv_tick_inc(10); lv_timer_handler(); }
static uint8_t grey(lv_color32_t c)
{ return (uint8_t)((77u * c.red + 150u * c.green + 29u * c.blue) >> 8); }

// Preserve the actual I1 palette and pixels. The surrounding quiet zone uses
// the real card's background, rather than adding an invented white border.
static uint8_t *raster(lv_obj_t *qr, bool control, int *side)
{
    lv_obj_t *card = lv_obj_get_parent(qr);
    lv_obj_update_layout(card);
    lv_draw_buf_t *buf = lv_canvas_get_draw_buf(qr);
    check(buf && buf->header.cf == LV_COLOR_FORMAT_I1, "QR owns an I1 canvas");
    int n = lv_obj_get_width(card), w = (int)buf->header.w;
    check(n >= w && buf->header.h == buf->header.w, "square QR fits card");
    uint8_t *out = malloc((size_t)n * n);
    check(out != NULL, "allocate raster");
    lv_color32_t *palette = (lv_color32_t *)buf->data;
    const uint8_t *pixels = lv_draw_buf_goto_xy(buf, 0, 0);
    uint8_t light = control ? 255 : grey(palette[0]);
    uint8_t dark = control ? 0 : grey(palette[1]);
    memset(out, light, (size_t)n * n);
    int offset = (n - w) / 2;
    for (int y = 0; y < w; ++y)
        for (int x = 0; x < w; ++x)
            out[(size_t)(y + offset) * n + x + offset] =
                pixels[(size_t)y * buf->header.stride + (x >> 3)] & (0x80u >> (x & 7)) ? dark : light;
    *side = n;
    return out;
}
static bool decode_raster(k_quirc_t *decoder, const uint8_t *image, int side,
                           const void *expected, size_t len, int version)
{
    check(k_quirc_resize(decoder, side, side) == 0, "resize camera decoder");
    uint8_t *target = k_quirc_begin(decoder, NULL, NULL);
    check(target != NULL, "camera image buffer");
    memcpy(target, image, (size_t)side * side);
    k_quirc_end(decoder, false);
    for (int i = 0; i < k_quirc_count(decoder); ++i) {
        k_quirc_result_t result;
        if (k_quirc_decode(decoder, i, &result) == K_QUIRC_SUCCESS &&
            result.data.payload_len == (int)len &&
            memcmp(result.data.payload, expected, len) == 0 &&
            (!version || result.data.version == version)) return true;
    }
    return false;
}
static void check_image(lv_obj_t *qr, const void *expected, size_t len, int version)
{
    k_quirc_t *reader = k_quirc_new();
    check(reader != NULL, "create camera decoder");
    for (int control = 0; control <= 1; ++control) {
        int side;
        uint8_t *image = raster(qr, control != 0, &side);
        check(decode_raster(reader, image, side, expected, len, version),
              control ? "native black/white raster decodes exactly" : "native actual-palette raster decodes exactly");
        free(image);
    }
    k_quirc_destroy(reader);
}
static lv_obj_t *new_card(lv_obj_t **qr)
{
#if KISS_NARROW
    return wt_qr_card_bare(lv_screen_active(), qr, 18, 36, 216, 206);
#else
    return wt_qr_card_bare(lv_screen_active(), qr, SX(48), SY(96), SQ(302), SQ(274));
#endif
}
static uint8_t *snapshot(lv_obj_t *qr, size_t *n)
{
    lv_draw_buf_t *b = lv_canvas_get_draw_buf(qr);
    *n = b->data_size;
    uint8_t *copy = malloc(*n);
    check(copy != NULL, "allocate bitmap snapshot");
    memcpy(copy, b->data, *n);
    return copy;
}
static void unchanged(lv_obj_t *qr, const uint8_t *copy, size_t n)
{
    lv_draw_buf_t *b = lv_canvas_get_draw_buf(qr);
    check(b->data_size == n && !memcmp(b->data, copy, n), "rejected update preserves bitmap");
}
static void lifecycle(void)
{
    lv_obj_t *qr, *card = new_card(&qr);
    const uint8_t binary[] = {'p','s','b','t',0xff,0,'A',0,'B'};
    check(wt_qr_update(qr, binary, sizeof binary) == LV_RESULT_OK, "binary embedded NUL accepted");
    check_image(qr, binary, sizeof binary, 0);
    const char tiny[] = "UR:CRYPTO-PSBT/AEAE";
    check(wt_qr_update(qr, tiny, sizeof tiny - 1) == LV_RESULT_OK, "baseline tiny binary render");
    size_t tiny_n;
    uint8_t *tiny_pixels = snapshot(qr, &tiny_n);
    check(wt_qr_update_ur(qr, tiny, sizeof tiny - 1, 6, true) == LV_RESULT_OK, "tiny single-part render");
    unchanged(qr, tiny_pixels, tiny_n);
    free(tiny_pixels);

    const char normal[] = "UR:CRYPTO-PSBT/1-12/LPADBGCSIECYGHKSWPDEHDAMAEAOAXAA";
    check(wt_qr_update_ur(qr, normal, sizeof normal - 1, 6, false) == LV_RESULT_OK, "fixed-version render");
    size_t n;
    uint8_t *before = snapshot(qr, &n);
    check(wt_qr_update_ur(qr, NULL, 2, 6, false) == LV_RESULT_INVALID, "null UR rejected");
    check(wt_qr_update_ur(qr, normal, 0, 6, false) == LV_RESULT_INVALID, "empty UR rejected");
    check(wt_qr_update_ur(qr, "UR:CRYPTO-PSBT/", 15, 6, false) == LV_RESULT_INVALID, "empty UR suffix rejected");
    check(wt_qr_update_ur(qr, normal, sizeof normal, 6, false) == LV_RESULT_INVALID, "embedded terminator rejected");
    check(wt_qr_update_ur(qr, normal, sizeof normal - 1, 7, false) == LV_RESULT_INVALID, "unsupported version rejected");
    check(wt_qr_update_ur(qr, "ur:crypto-psbt/aeae", 19, 6, false) == LV_RESULT_INVALID, "lowercase rejected");
    check(wt_qr_update_ur(qr, normal, UINT32_MAX, 6, false) == LV_RESULT_INVALID, "oversized length rejected before read");
    char huge[599]; memset(huge, 'A', sizeof huge); memcpy(huge, "UR:CRYPTO-PSBT/", 15);
    check(wt_qr_update_ur(qr, huge, sizeof huge, 6, false) == LV_RESULT_INVALID, "profile capacity overflow rejected");
    unchanged(qr, before, n);
    free(before);

    wt_qr_zoom(qr);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *overlay = lv_obj_get_child(screen, -1);
    check(overlay != card, "zoom overlay opens after rejected update");
    lv_obj_t *zoom_qr = lv_obj_get_child(lv_obj_get_child(overlay, 0), 0);
    check_image(zoom_qr, normal, sizeof normal - 1, 6);
    const char latest[] = "UR:CRYPTO-PSBT/2-12/LPADBGCSIECYGHKSWPDEHDAMAEAOAXAA";

    // Exhaust only LVGL's bounded simulator heap. A QR work buffer needs
    // more than 256 bytes, so both rendering and closing must fail safely.
    uint8_t *zoom_before = snapshot(zoom_qr, &n);
    void *held[1024];
    size_t held_count = 0;
    while (held_count < sizeof held / sizeof held[0]) {
        void *block = lv_malloc(256);
        if (!block) break;
        held[held_count++] = block;
    }
    check(held_count < sizeof held / sizeof held[0], "exhaust bounded LVGL heap");
    check(wt_qr_update_ur(qr, latest, sizeof latest - 1, 8, false) == LV_RESULT_INVALID,
          "work-buffer allocation failure rejects update");
    char larger[180]; memset(larger, 'A', sizeof larger); memcpy(larger, "UR:CRYPTO-PSBT/", 15);
    check(wt_qr_update_ur(qr, larger, sizeof larger, 8, false) == LV_RESULT_INVALID,
          "cache allocation failure rejects update");
    check(wt_qr_update_ur(qr, larger, sizeof larger, 9, true) == LV_RESULT_INVALID,
          "failed UR-to-binary transition preserves the cached UR frame");
    unchanged(zoom_qr, zoom_before, n);
    lv_obj_send_event(overlay, LV_EVENT_CLICKED, NULL);
    for (size_t i = 0; i < held_count; ++i) lv_free(held[i]);
    service();
    check(lv_obj_is_valid(overlay) && lv_obj_get_child(screen, -1) == overlay,
          "failed redraw leaves zoom open after async deletions");
    check(lv_mem_test() == LV_RESULT_OK, "LVGL heap remains valid after failed render");
    free(zoom_before);
    check_image(zoom_qr, normal, sizeof normal - 1, 6);
    lv_obj_send_event(overlay, LV_EVENT_CLICKED, NULL);
    service();
    check(!lv_obj_is_valid(overlay), "recovered close removes original zoom overlay");
    check_image(qr, normal, sizeof normal - 1, 6);
    wt_qr_zoom(qr);
    overlay = lv_obj_get_child(screen, -1);
    check(overlay != card, "zoom reopens after recovering from OOM");
    zoom_qr = lv_obj_get_child(lv_obj_get_child(overlay, 0), 0);

    check(wt_qr_update_ur(qr, latest, sizeof latest - 1, 8, false) == LV_RESULT_OK, "zoom updates cached profile");
    check_image(zoom_qr, latest, sizeof latest - 1, 8);
    lv_obj_send_event(overlay, LV_EVENT_CLICKED, NULL);
    service();
    check_image(qr, latest, sizeof latest - 1, 8);

    // This text exceeds v6-M's byte capacity, but fits its alphanumeric one.
    char single[120]; memset(single, 'A', sizeof single); memcpy(single, "UR:CRYPTO-PSBT/", 15);
    check(wt_qr_update_ur(qr, single, sizeof single, 6, true) == LV_RESULT_OK, "larger single uses alphanumeric capacity");
    check_image(qr, single, sizeof single, 6);
    check(wt_qr_update(qr, binary, sizeof binary) == LV_RESULT_OK, "return from UR to generic binary");
    wt_qr_zoom(qr);
    overlay = lv_obj_get_child(screen, -1);
    check(overlay != card, "binary zoom opens after fixed UR");
    zoom_qr = lv_obj_get_child(lv_obj_get_child(overlay, 0), 0);
    check_image(zoom_qr, binary, sizeof binary, 0);
    wt_qr_scrub(qr);
    check(wt_sim_qr_texts(NULL, 0) == 0, "scrub clears cached payload");
    lv_obj_send_event(overlay, LV_EVENT_CLICKED, NULL);
    service();
    lv_obj_delete(card);
    service();
    printf("PASS lifecycle: binary NUL, tiny single, capacity, invalid/OOM update, zoom/cache/profile, scrub\n");
}
static void save_raster(lv_obj_t *qr, const char *name, int frame, const char *part)
{
    if (!dump_dir) return;
    int side; uint8_t *image = raster(qr, false, &side);
    char path[1024];
    snprintf(path, sizeof path, "%s/%s-%04d.pgm", dump_dir, name, frame);
    FILE *file = fopen(path, "wb"); check(file != NULL, "open PGM output");
    fprintf(file, "P5\n%d %d\n255\n", side, side);
    check(fwrite(image, 1, (size_t)side * side, file) == (size_t)side * side, "write PGM");
    fclose(file); free(image);
    snprintf(path, sizeof path, "%s/%s-%04d.txt", dump_dir, name, frame);
    file = fopen(path, "wb"); check(file != NULL, "open payload output");
    fputs(part, file); fclose(file);
}
static void roundtrip(const uint8_t *raw, size_t len, int fragment, int version,
                       const char *name)
{
    qrt_encoder_t *encoder = qrt_encoder_new_frag(QRT_FMT_UR, raw, len, fragment);
    ur_decoder_t *decoder = ur_decoder_new();
    k_quirc_t *reader = k_quirc_new();
    check(encoder && decoder && reader, "roundtrip objects allocate");
    int parts = qrt_encoder_parts(encoder);
    lv_obj_t *qr, *card = new_card(&qr);
    char part[600];
    for (int frame = 0; frame < parts; ++frame) {
        check(qrt_encoder_next(encoder, part, sizeof part) == 0, "encode UR frame");
        check(wt_qr_update_ur(qr, part, (uint32_t)strlen(part), version, parts == 1) == LV_RESULT_OK, "render UR frame");
        for (int control = 0; control <= 1; ++control) {
            int side; uint8_t *image = raster(qr, control != 0, &side);
            bool read = decode_raster(reader, image, side, part, strlen(part), parts == 1 ? 0 : version);
            if (!read) fprintf(stderr, "%s: frame %d/%d %s palette\n", name, frame + 1, parts, control ? "control" : "actual");
            check(read, "every native rendered UR frame decodes byte-exact");
            free(image);
        }
        // Only raster-verified text is fed into assembly; exact equality above
        // also proves no QR rendering/reading corruption was hidden by cUR.
        check(ur_decoder_receive_part(decoder, part), "assemble raster-decoded frame");
        save_raster(qr, name, frame, part);
    }
    check(ur_decoder_is_success(decoder), "native raster sequence completes");
    ur_result_t *result = ur_decoder_get_result(decoder);
    check(result && !strcmp(result->type, "crypto-psbt"), "result is crypto-psbt");
    psbt_data_t *data = psbt_from_cbor(result->cbor_data, result->cbor_len);
    size_t decoded_len = 0;
    const uint8_t *decoded = data ? psbt_get_data(data, &decoded_len) : NULL;
    check(decoded && decoded_len == len && !memcmp(decoded, raw, len), "reassembled bytes exactly equal input");
    printf("PASS %s: %zu bytes, %d native raster frames, profile v%d, exact complete payload\n", name, len, parts, version);
    psbt_free(data); ur_decoder_free(decoder); qrt_encoder_free(encoder); k_quirc_destroy(reader);
    lv_obj_delete(card); service();
}
static void late_sequences(const uint8_t *raw, size_t len, int fragment,
                            int version, const char *name)
{
    // cUR exposes its sequence counter. Jump directly to real late fountain
    // frames instead of spending billions of iterations reaching them.
    static const uint32_t sequence[] = {9999, 10000, 65535, 65536, 1000000, UINT32_MAX};
    psbt_data_t *psbt = psbt_new(raw, len);
    size_t cbor_len = 0;
    uint8_t *cbor = psbt ? psbt_to_cbor(psbt, &cbor_len) : NULL;
    ur_encoder_t *encoder = cbor ? ur_encoder_new("crypto-psbt", cbor, cbor_len, fragment, 0, 10) : NULL;
    check(encoder != NULL, "late-sequence encoder allocates");
    free(cbor); psbt_free(psbt);
    lv_obj_t *qr, *card = new_card(&qr);
    for (size_t i = 0; i < sizeof sequence / sizeof sequence[0]; ++i) {
        encoder->fountain_encoder->seq_num = sequence[i] - 1;
        char *part = NULL;
        check(ur_encoder_next_part(encoder, &part) && part, "encode late sequence");
        char prefix[64];
        snprintf(prefix, sizeof prefix, "UR:CRYPTO-PSBT/%" PRIu32 "-", sequence[i]);
        check(strncmp(part, prefix, strlen(prefix)) == 0, "actual encoded late sequence number");
        check(wt_qr_update_ur(qr, part, (uint32_t)strlen(part), version, false) == LV_RESULT_OK,
              "late sequence fits fixed profile");
        check_image(qr, part, strlen(part), version);
        save_raster(qr, name, (int)i, part);
        free(part);
    }
    char *wrapped = NULL;
    check(!ur_encoder_next_part(encoder, &wrapped) && !wrapped, "sequence exhaustion does not wrap");
    printf("PASS %s: %zu bytes, six late sequences through UINT32_MAX, profile v%d\n", name, len, version);
    ur_encoder_free(encoder); lv_obj_delete(card); service();
}
int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    const char *extra = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--file") && i + 1 < argc) extra = argv[++i];
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump_dir = argv[++i];
        else { fprintf(stderr, "usage: %s [--file PSBT] [--dump existing-directory]\n", argv[0]); return 2; }
    }
    lv_init();
    lv_display_t *display = lv_display_create(SCREEN_W, SCREEN_H);
    static uint8_t display_pixels[SCREEN_W * 16 * 2];
    lv_display_set_buffers(display, display_pixels, NULL, sizeof display_pixels, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lifecycle();
    uint8_t raw[QRT_MAX_SIGNED_PSBT];
    uint32_t state = 0x712a43d5;
    for (size_t i = 0; i < sizeof raw; ++i) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        raw[i] = (uint8_t)state;
    }
    memcpy(raw, "psbt\xff", 5);
#if KISS_NARROW
    const int normal = 60, normal_v = 8, easy = 30, easy_v = 6;
#else
    const int normal = 180, normal_v = 13, easy = 90, easy_v = 9;
#endif
    roundtrip(raw, 24, normal, normal_v, "tiny");
    roundtrip(raw, 4405, normal, normal_v, "growth-normal");
    roundtrip(raw, 4405, easy, easy_v, "growth-easy");
    roundtrip(raw, sizeof raw, normal, normal_v, "boundary-normal");
    roundtrip(raw, sizeof raw, easy, easy_v, "boundary-easy");
    late_sequences(raw, sizeof raw, normal, normal_v, "late-normal");
    late_sequences(raw, sizeof raw, easy, easy_v, "late-easy");
#if !KISS_NARROW
    // 8997 raw bytes become exactly 9000 CBOR bytes: all 50 fragments use the
    // full 180-byte allowance (the 9108 boundary has 51 shorter fragments).
    late_sequences(raw, 8997, normal, normal_v, "late-full-normal");
#endif
    if (extra) {
        FILE *file = fopen(extra, "rb"); check(file != NULL, "open optional PSBT");
        size_t n = fread(raw, 1, sizeof raw, file);
        check(n > 0 && !ferror(file) && fgetc(file) == EOF, "optional PSBT fits signed bound");
        fclose(file);
        roundtrip(raw, n, normal, normal_v, "file-normal");
        roundtrip(raw, n, easy, easy_v, "file-easy");
    }
    service();
    lv_display_delete(display);
    lv_deinit();
    printf("PASS: %u checks at %dx%d\n", checks, SCREEN_W, SCREEN_H);
    return 0;
}
