# Vendored: waveshare/esp_lcd_hx8394, as trimmed by odudex/Kern

- Upstream: https://github.com/waveshareteam/Waveshare-ESP32-components
  (`display/lcd/esp_lcd_hx8394`), published to the component registry as
  `waveshare/esp_lcd_hx8394`.
- Taken from: Kern's `components/wave_5/esp_lcd_hx8394.c` and
  `include/esp_lcd_hx8394.h` at Kern commit `cbd9802` (2026-09-21). That copy is
  Waveshare's 1.0.3 with two changes: `color_space` renamed `rgb_ele_order` for
  ESP-IDF 6, and the driver's own I2C bus and power rail write removed. Checked
  2026-10-01 against registry 2.1.0: the 21 row init table is byte identical.
- License: MIT. LICENSE is upstream's `license.txt` as shipped, placeholders and
  all; Kern's tree is MIT too.
- Local changes: the header comment names this tree. Nothing in the code.

## Why vendored rather than pinned from the registry

Registry 2.1.0 pulls `espressif/i2c_bus` and `cmake_utilities`, and unless
`CONFIG_ESP_LCD_HX8394_SKIP_I2C_INIT` is set it opens the LEGACY I2C driver on
GPIO 7 and 8, the pins this firmware's new-style bus already owns for touch and
the camera's SCCB, writes the panel's power rail chip at 0x45 through it and
sleeps a second. Two I2C drivers on one pair of pins is a build that works until
it does not, behind a Kconfig somebody has to remember. The power rail write
moved to `main/board_ws5.c`, on the shared bus, probe first.

## Not taken from 2.1.0

- `esp_lcd_dpi_panel_enable_dma2d` inside the constructor: the board file
  decides, as it does for the other DSI panels here.
- `HX8394_720_1280_PANEL_30HZ_DPI_CONFIG`: `board_ws5.c` writes the timing out
  beside the other boards' numbers.
- The I2C block, for the reason above.
