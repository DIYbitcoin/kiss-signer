# Vendored: espressif/esp_cam_sensor

- Upstream: https://components.espressif.com/components/espressif/esp_cam_sensor
- License: Apache-2.0, Espressif authored.
- Why vendored: the registry component lacks OV02C10, which is the sensor
  Guition actually ships on the JC4880P443C camera ribbon. Wired in through
  `override_path` in [main/idf_component.yml](../../main/idf_component.yml).

Every local change is listed here. Anyone updating this component has to carry
these forward or knowingly drop them.

## 1. `project_include.cmake`: register the OV02C10 tuning file

**Added** a `CONFIG_CAMERA_OV02C10` block at the top of the file.

Upstream ships `sensors/ov02c10/cfg/ov02c10_default.json`, a complete AWB, AGC,
CCM, denoise and gamma tuning file, and a Kconfig choice that selects it by
default. It then never registers it in `project_include.cmake`. Fourteen other
sensors have a block there; OV02C10 had none.

The effect on a build that selects OV02C10 is silent and total: the JSON never
reaches `ESP_IPA_JSON_CONFIG_FILE_PATH`, so the image processing algorithms
come up with no parameters for the sensor in the device. Nothing warns. It
looks exactly like a camera whose auto exposure is simply poor.

The added block is a copy of the OV5647 block above it, with the paths changed.

Worth reporting upstream.

## 2. `sensors/ov02c10/private_include/ov02c10_settings.h`: zero the black pedestal

**Changed** register `0x4003` from `0x40` to `0x00`, at both sites, in
`ov02c10_input_24M_MIPI_1lane_raw10_1288x728_30fps[]` and
`ov02c10_input_24M_MIPI_1lane_raw10_1920x1080_30fps[]`.

`0x4003` is the sensor's black level target: a pedestal added to every pixel
before it leaves the sensor. The ESP32-P4 ISP has a black level correction
block that could subtract it, but `ov02c10_default.json` carries no `blc`
section, so the pipeline controller never programs one and the offset survives
into RGB565 as lifted blacks and flat contrast.

Zeroed at source rather than cancelled downstream because the offset is per
channel, and one global subtraction cannot undo a per channel lift: it trades a
grey cast for a coloured one.

This matters beyond how the preview looks. The camera entropy meter in
[main/camera_spike.c](../../main/camera_spike.c) estimates Shannon entropy from
a histogram of raw pixel values, and a histogram is order blind. A fixed
pedestal is identical on every frame of every device and contributes no
unpredictability, but it widens the histogram and raises the reading. It was
inflating a number the firmware uses to tell the holder their seed is ready.

## Not changed, and why

- **No gamma LUT written from firmware.** An earlier plan had us programming a
  2.2 tone curve through `V4L2_CID_USER_ESP_ISP_GAMMA`. The tuning file already
  carries `aen.gamma` with `use_gamma_param: true` and drives it from
  `ae.luma.avg`, so once the pipeline controller is enabled the IPA owns gamma.
  Writing our own would fight it every frame, and gamma and the AE setpoint are
  coupled: AE meters luma AFTER gamma, so moving one without the other regresses
  exposure. Left to the tuning file.
- **Lens shading correction is still off.** `ov02c10_default.json` has no `lsc`
  section, so the ISP shading block is never programmed and vignetting is
  uncorrected. Fixing it needs per lens gain coefficients measured on a flat
  field, per lens type, which we do not have. It is the remaining fixed pattern
  inflating the entropy reading.
- **Per session AE metering weights** (centre weighted while scanning a QR,
  flat while gathering entropy) are not implemented. They need the pipeline
  controller's ownership of the sensor handed over temporarily, which is a
  larger change than this one. Deferred.

## Credit

The diagnosis that OV02C10 on the ESP32-P4 runs with an unconfigured ISP and
an uncorrected pedestal came from
https://github.com/kdmukai/esp-board-common/pull/18 (MIT), on the same sensor
and the same SoC. The warning about gamma and the AE setpoint being coupled is
theirs. The changes here were re-derived against this tree.
