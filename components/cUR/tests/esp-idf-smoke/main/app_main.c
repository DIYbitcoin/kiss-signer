// ESP-IDF smoke test: instantiates decoder and encoder to force the linker
// to resolve every symbol the public API touches. The build succeeding is
// the actual assertion; the runtime checks are a second line of defence
// that will fail loudly on a board if anything regresses.

#include "esp_log.h"
#include "types/bytes_type.h"
#include "ur_decoder.h"
#include "ur_encoder.h"
#include <stdlib.h>

static const char *TAG = "cur_smoke";

void app_main(void) {
  ESP_LOGI(TAG, "cUR smoke test: starting");

  ur_decoder_t *dec = ur_decoder_new();
  if (!dec) {
    ESP_LOGE(TAG, "ur_decoder_new returned NULL");
    return;
  }

  // Negative path: decoder must reject malformed input without crashing.
  if (ur_decoder_receive_part(dec, "not-a-ur")) {
    ESP_LOGE(TAG, "decoder accepted malformed input");
    ur_decoder_free(dec);
    return;
  }
  ur_decoder_free(dec);

  // Encoder round-trip over a one-byte CBOR byte string (0x41 0xaa).
  const uint8_t cbor[] = {0x41, 0xaa};
  ur_encoder_t *enc = ur_encoder_new("bytes", cbor, sizeof(cbor),
                                     /*max_fragment_len=*/200,
                                     /*first_seq_num=*/0,
                                     /*min_fragment_len=*/10);
  if (!enc) {
    ESP_LOGE(TAG, "ur_encoder_new returned NULL");
    return;
  }

  char *part = NULL;
  if (!ur_encoder_next_part(enc, &part) || !part) {
    ESP_LOGE(TAG, "ur_encoder_next_part failed");
    ur_encoder_free(enc);
    return;
  }
  ESP_LOGI(TAG, "encoded part: %s", part);
  free(part);
  ur_encoder_free(enc);

  ESP_LOGI(TAG, "cUR smoke test: PASS");
}
