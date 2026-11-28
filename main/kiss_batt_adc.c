// The battery on the Waveshare 4.3in and 5in, which have no power chip: an
// ETA6098 charges the cell and tells the P4 nothing, and the cell's voltage is
// the one thing wired back. BAT --[200k]-- BAT_ADC --[100k]-- GND, on GPIO20
// (ADC1 channel 4) on both boards, so a full 4.2 V cell puts 1.4 V on the pin.
// Waveshare's 5in schematic draws it so; their 4.3in schematic draws GPIO2
// behind an unfitted resistor, and Kern (MIT, components/bsp_common/
// pmic_adc.c) reads GPIO20 on both and says it was verified on both boards.
//
// So this reads a voltage and turns it into a percent with a lithium cell's
// curve, which is good to about ten percent on the battery. Nothing senses the
// USB cable or the charger, so `usb` and `charging` stay false, and on the
// cable the charger lifts the voltage and the percent reads high.
//
// Asked lazily from the home's poll, like the 3.5in's chip, so nothing here
// sits in the boot path; an ADC that does not come up is dropped once.
#include "kiss_board.h"

#if KISS_PMIC && (defined(KISS_BOARD_WS43) || defined(KISS_BOARD_WS5))

#include <string.h>
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "batt";

#define BAT_GPIO     20
#define BAT_DIVIDER  3      // 200k over 100k
#define BAT_SAMPLES  16     // the divider node is noisy; Kern averages 16 too
#define BAT_NONE_MV  2500   // below this the pin is not following a cell
// With no cell fitted and USB in, the charger may hold the BAT node near its
// 4.2 V charge voltage, which reads as a full battery that is not there, and
// nothing else on the board says whether a cell is fitted. So the badge waits
// until the voltage has been below this once since boot, which only a cell
// does: a full cell shows from about 90%, and a board with no cell never does.
#define BAT_SEEN_MV  4100

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static bool s_gone;
static int s_mv_avg;        // smoothed cell voltage, 0 until the first read

static bool adc_up(void) {
  if (s_cali) return true;
  if (s_gone) return false;
  adc_unit_t unit;
  if (adc_oneshot_io_to_channel(BAT_GPIO, &unit, &s_chan) != ESP_OK) goto fail;
  adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = unit};
  if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) goto fail;
  adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
  adc_cali_curve_fitting_config_t cal = {.unit_id = unit, .chan = s_chan,
                                         .atten = ADC_ATTEN_DB_12,
                                         .bitwidth = ADC_BITWIDTH_DEFAULT};
  if (adc_oneshot_config_channel(s_adc, s_chan, &ccfg) != ESP_OK ||
      adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) {
    adc_oneshot_del_unit(s_adc);
    s_adc = NULL;
    s_cali = NULL;
    goto fail;
  }
  return true;
fail:
  ESP_LOGW(TAG, "battery: no ADC on GPIO%d", BAT_GPIO);
  s_gone = true;
  return false;
}

// A lithium cell's resting voltage against how full it is, under the light
// load this board draws. Straight lines between the points.
static const struct { int mv; int pct; } k_curve[] = {
  {4200, 100}, {4100, 90}, {4000, 80}, {3920, 70}, {3850, 60}, {3800, 50},
  {3750, 40}, {3710, 30}, {3670, 20}, {3600, 10}, {3400, 0},
};

static unsigned char mv_to_pct(int mv) {
  const int n = (int)(sizeof k_curve / sizeof k_curve[0]);
  if (mv >= k_curve[0].mv) return 100;
  for (int i = 1; i < n; i++) {
    if (mv >= k_curve[i].mv) {
      int hi_mv = k_curve[i - 1].mv, hi_p = k_curve[i - 1].pct;
      int lo_mv = k_curve[i].mv, lo_p = k_curve[i].pct;
      return (unsigned char)(lo_p + (hi_p - lo_p) * (mv - lo_mv) / (hi_mv - lo_mv));
    }
  }
  return 0;
}

bool kiss_board_batt_read(kiss_batt_t *b) {
  if (!b) return false;
  memset(b, 0, sizeof *b);
  if (!adc_up()) return false;
  int sum = 0;
  for (int i = 0; i < BAT_SAMPLES; i++) {
    int node_mv;
    if (adc_oneshot_get_calibrated_result(s_adc, s_cali, s_chan, &node_mv) != ESP_OK)
      return false;
    sum += node_mv;
  }
  int mv = sum / BAT_SAMPLES * BAT_DIVIDER;
  // Smoothed across polls so the percent does not jump with the screen's
  // load; a jump of more than 300 mV is a cell plugged or pulled, taken as is.
  if (s_mv_avg == 0 || mv - s_mv_avg > 300 || s_mv_avg - mv > 300) s_mv_avg = mv;
  else s_mv_avg += (mv - s_mv_avg) / 4;
  static bool logged;
  if (!logged) {
    logged = true;
    ESP_LOGI(TAG, "battery: %d mV on GPIO%d", mv, BAT_GPIO);
  }
  static bool seen;
  if (s_mv_avg >= BAT_NONE_MV && s_mv_avg < BAT_SEEN_MV) seen = true;
  b->present = seen && s_mv_avg >= BAT_NONE_MV;
  b->pct = b->present ? mv_to_pct(s_mv_avg) : 0;
  return true;
}

#endif
