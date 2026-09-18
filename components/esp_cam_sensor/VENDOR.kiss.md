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

## 3. `sensors/ov02c10/ov02c10.c`: fill in `tline_ns`

**Added** `.tline_ns` to all three entries of `ov02c10_isp_info[]`: 27918 for
the two 1-lane formats and 13959 for the 2-lane one.

Upstream never sets the field, so it is zero. Zero is not a cosmetic default
here: it is what stops the ISP pipeline from starting at all. The controller
converts the sensor's exposure limits from register units into microseconds by
multiplying through `tline_ns`
(`esp_video_isp_pipeline.c`, `REG_TO_US`, in the esp_video component the
build fetches), so min, max and current exposure all collapse to 0 and the AGC
refuses to initialise. `esp_video_init` then fails with a flat
`ESP_ERR_NOT_SUPPORTED`, taking the whole camera down, scanning included.

This is the same upstream gap as the missing registration in section 1, seen
from the other end. OV5647 also has no `tline_ns`, and its tuning file
correspondingly has no `agc` and no `awb` section. OV02C10's tuning file *does*
carry both. So the JSON was written for a driver that never supplied the line
time it needs.

The value is `hts / pclk`. SC2336 is what settles the convention: its first
three entries share `hts` and `pclk` and one `tline_ns` of 22222 across three
different `vts` values, so the field tracks line readout and not frame rate.

- 1-lane: 2280 / 81666700 = 27.918 us
- 2-lane: 1140 / 81666700 = 13.959 us

Cross-check: 1164 lines x 27.918 us and 2328 lines x 13.959 us both come to
32.5 ms per frame, which is the same sensor running at the same real rate
through a different lane count. Note that 32.5 ms is 30.8 fps, not the 30 the
format table declares; the declared figure is nominal.

Worth reporting upstream, with section 1.

## 4. The pipeline controller is off, and what it did when it was on

Nothing in this component changes for this. It is recorded here because the
three sections above exist to make the ISP pipeline controller work, and after
all three landed it still had to be switched back off in
[sdkconfig.defaults](../../sdkconfig.defaults). Anyone reading sections 1 to 3
would otherwise reasonably assume the controller is running.

With `CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER=y` the pipeline started
cleanly, so sections 1 and 3 did their job. The AGC then drove sensor exposure
to the floor, 8 lines, `s_ov02c10_exp_min` in `ov02c10.c`, which is 223 us, and
left it there with gain near the top of the map. Measured on device by reading
`V4L2_CID_EXPOSURE` and `V4L2_CID_GAIN` back every fifteen frames.

Three things say this is the controller and not the sensor:

- The pipeline itself writes `qctrl.default_value` at init, which for every
  format in this driver is `exp_def`, 0x46c, 1132 lines, 31.6 ms. So exposure
  starts one line short of a full frame and the AGC takes it down by a factor
  of 140.
- Both numbers held still when the room light was switched on. A working AGC
  moves. Gain was not at its cap either, index 191 of 197, so it had headroom
  it did not use.
- Off, the sensor holds `exp_def` and the same device scans QR codes and reads
  over 6.0 bits of camera entropy, which is what it did before any of this.

The consequence was not cosmetic. `ENT_THRESH_X10` in
[main/camera_spike.c](../../main/camera_spike.c) gates seed creation at 6.0
bits and a 223 us frame measured 3.5, so the holder could not finish creating a
wallet at all.

Two suspects, neither confirmed, both cheap to test when someone picks this up:

- `agc.mode` is `high_light_priority`, which meters to protect highlights. In a
  dim room holding one bright thing, that is the mode that drives everything
  else black. SC2336, which is known good on this SoC, uses
  `light_threshold_priority`, and `ov02c10_default.json` already carries a
  fully populated `light_threshold_priority` array, so trying it is one word.
- `agc.anti_flicker.mode` is `part`, which per the esp_ipa README forces
  anti-flicker exposure quantisation whenever gain can still carry the
  brightness. At `ac_freq` 50 the quantum is 10 ms, and this format's whole
  frame is 32.5 ms. `none` is the comparison. Note that esp_ipa 2.2.0~1, the
  version here, already contains the 1.1.0 fix for gain saturation in this
  mode, so this is the weaker of the two.

The AWB half was never separately assessed, because the exposure fault made
every frame too dark to judge white balance on.

## 5. OV5647 on the 3.5in: nothing changed, and the controller stays off

Nothing in this component changes for the Waveshare 3.5in board either. Its
camera is an OV5647, selected in [sdkconfig.ws35](../../sdkconfig.ws35) in the
upstream `MIPI_2lane_24Minput_RAW10_1280x960_binning_45fps` mode, the one Kern
runs on the same board, and every register it writes is upstream's.

`CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER` stays `n` for this sensor
as a decision of its own, not only because the board profile inherits it:

- The sensor meters itself. `ov5647_set_format` resets it and never writes
  `0x3503`, so exposure and gain stay under the chip's own AEC and AGC, with
  the target window `ov5647_set_AE_target` writes (0x50) and the banding
  filter on top. Section 4's problem, a sensor holding one fixed exposure for
  the whole session, does not arise.
- The controller would have nothing to run on. `ov5647_default.json` carries
  no `agc` and no `awb` section, and `ov5647_isp_info[]` has no `tline_ns`, the
  gap section 3 filled for OV02C10.

Three consequences, all still to be read on glass:

- `V4L2_CID_EXPOSURE` is not a line count here. esp_video maps it to
  `ESP_CAM_SENSOR_EXPOSURE_VAL`, which this driver implements as the on-chip
  AE target (2..235, default 0x50) and cannot read back. `scan_exposure` in
  [main/camera_spike.c](../../main/camera_spike.c) therefore halves the target
  while scanning, 0x50 to 0x28, and the sensor meets it with a shorter
  exposure in good light and less gain in poor light. The scan preview is
  meant to be darker than the entropy page's.
- No white balance gains are programmed without the controller, so colour is
  the sensor's raw balance. The QR decoder reads the green channel and the
  entropy meter a histogram of RGB565 values, and neither needs colour to be
  right; but the entropy floor and target in camera_spike.c were set against
  OV02C10 frames and have not been measured on this sensor.
- The mode table sets `0x5000` to `0xff`, which turns on the sensor's own lens
  correction at its default coefficients. Those were not measured for this
  lens either, so how much of the vignetting note below holds on the 3.5in
  is unmeasured.

## Not changed, and why

- **No gamma LUT written from firmware.** An earlier plan had us programming a
  2.2 tone curve through `V4L2_CID_USER_ESP_ISP_GAMMA`. The tuning file already
  carries `aen.gamma` with `use_gamma_param: true` and drives it from
  `ae.luma.avg`, so whenever the pipeline controller runs the IPA owns gamma.
  Writing our own would fight it every frame, and gamma and the AE setpoint are
  coupled: AE meters luma AFTER gamma, so moving one without the other regresses
  exposure. Left to the tuning file. With the controller off, per section 4, no
  gamma is programmed at all and the output is the sensor's own curve, which is
  what shipped before this work and what ships now.
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
