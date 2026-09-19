# NU-87 / NU-IMU bring-up and calibration

Status: implementation and build validation; **NU-87 + NU-IMU bench verification pending**.
Tracks [Issue #17](https://github.com/mzsz94/micro-wallaby/issues/17).

## Hardware identity and wiring

The supplied [NUCODE product page](https://nucode.store/product/detail.html?product_no=37)
identifies **NU-IMU-LSM6DS3TR**, a six-axis accelerometer/gyroscope module with
I2C/SPI and Qwiic connectors. Its published module input range is 3–5 V and
I2C addresses are 0x6a / 0x6b. This is a module rating, not permission to apply
5 V to the sensor's digital I/O or the NU-87 GPIOs.

Power off both boards before wiring. Use a common ground and short wires.

| NU-IMU signal | NU-87 connection | Note |
| --- | --- | --- |
| VCC | 3.3 V supply | Start with a 3.3 V supply within the module's published range |
| GND | GND | Required shared reference |
| SCL | PA25 | I2C0 clock, 100 kHz bring-up configuration |
| SDA | PA26 | I2C0 data |
| CS / SCS, if exposed and not already pulled high | 3.3 V logic | Select I2C, not SPI; verify module strap first |
| SDO / SA0 | Low selects 0x6a; high selects 0x6b | Inspect the SA0 solder bridge before changing anything |
| INT1 / INT2 | Unconnected | Polling experiment; interrupts are not used |

Do not connect both VCC and the separately labelled `3V3` pin to supplies. The
product photo shows both labels but is not a regulator schematic; do not assume
the `3V3` pin is an independent input. Qwiic is a 3.3 V interface: verify connector
pinout at both ends instead of relying only on wire colours.

The presence/value of onboard SDA/SCL pull-ups is not established by the product
photo. Verify them from the module schematic or with power-off measurements.
If missing, fit pull-ups to 3.3 V (typically 4.7 kOhm for a short 100 kHz bus).
Internal weak pulls are enabled for initial diagnosis but are not a qualified
substitute for correctly sized bus pull-ups. A spare 10 kOhm resistor is not a
specific requirement, and missing resistors must not be papered over in software.

PA25/PA26 also serve native USB on the SoC. This test uses the board's CP2102 USB
serial console; do not enable native USB on those two pins at the same time.
The old joystick and touch fixtures are not needed for this test.

## Chosen defaults and rationale

| Choice | Reason / limitation |
| --- | --- |
| Separate app and `feat/nu87-imu-bringup` branch | Isolate sensor work from the working Wi-Fi/NES and joystick applications |
| I2C, 100 kHz first | Four-wire integration and conservative breadboard signalling; 400 kHz can be a later measured change |
| Explicit `st,lsm6ds3` identity | The supplied product does not advertise the `-C` suffix; expect ID 0x69 and refuse writes on mismatch |
| Optional explicit `st,lsm6ds3tr-c` overlay | ID 0x6a is supported separately; do not silently identify an unknown 0x6a device as this model |
| Application-local Zephyr Sensor API driver | This pinned Zephyr tree has no dedicated LSM6DS3 driver; avoid pretending the part is an LSM6DSL |
| 104 Hz sensor ODR, 100 Hz polling target | 104 Hz is a native sensor rate. Fresh-data bits prevent repeated samples from being counted; this is not lossless FIFO capture |
| +/-2 g, +/-250 deg/s | Useful resolution for stationary and hand-motion tests, not a final locomotion range |
| BDU and one 12-byte gyro/accel read | Avoid torn low/high bytes and reduce bus overhead; no claim of hardware-synchronised FIFO timestamps |
| 10 Hz sample logging, 10 s health summaries | Keep console work below the 100 Hz acquisition load |
| 1000 fresh samples; manual gyro calibration | About 10 seconds of stationary data. Never subtract gravity from accelerometer readings |
| Manual save into NVS | Avoid writing flash every sample. CRC, record version and chip ID protect loading; temperature compensation and sensor serial-number binding are not implemented |

The LSM6DSL reboot issue is an upstream report, not proof that this NU-IMU has
the same fault. The local driver uses bounded SW_RESET and readback checks;
it does not skip sensor initialization on error.

## Software structure

```text
Devicetree (bus pins, address, explicit chip variant)
  -> app-local Sensor API adapter + portable register core
  -> 100 Hz main loop: fresh sample / error counters / SI-to-bench units
      -> 1000-sample Welford statistics and gyro bias subtraction
      -> shell requests: calibrate / save / clear / status / stream
      -> NVS record in existing storage partition, 0x250000..0x255fff
```

`imu_core.c` handles identity, bounded reset, configuration readback, fresh-data
checks, signed little-endian values and SI conversion. `imu_sensor.c` provides
Zephyr's `sensor_sample_fetch()` / `sensor_channel_get()` interface.
`imu_calibration.c` is portable and host-tested. `imu_storage.c` uses the Zephyr
flash map and NVS with data CRC; mounting does not automatically erase on error.
An empty first mount may initialize NVS metadata. The first accepted bias is
saved only by `imu save`; mounting an existing unrelated NVS partition is not a
migration strategy, so back up any pre-existing use of this reserved partition.

The I2C staging patch is deliberately separate:
`patches/zephyr/0005-i2c-amebad-bound-polling-and-preserve-restart.patch`.
The original polling path delays 5 ms per message, emits a STOP between register
address and data, and ignores HAL transfer counts. The patch enables hardware
restart, preserves message boundaries, serializes polling transfers, checks aborts
and applies a 50 ms transaction deadline. It is limited to AmebaD polling mode;
IRQ/DMA modes and other SoC families are unchanged. This is an experimental patch,
not an upstream submission or a hardware-validated driver fix.

`ameba_log_compat.c` supplies the missing `DiagVprintf` ABI for the pinned HAL's
non-Wi-Fi flash/I2C diagnostics. It does not enable radio code. Both compatibility
changes should be revisited against a future upstream baseline.

## Bench procedure (do not close Issue #17 before recording results)

1. Flash the package following [the Mac guide](imu-macos-flashing.md). Reset with
   BOOT released. Expect `WHO_AM_I=0x69 expected=0x69 init=0` at address 0x6a.
   On an ID mismatch, check the physical chip variant instead of disabling the
   check. Address 0x6b requires the SA0-high overlay and a separate build.
2. Put the module still on a table. `accel_g` magnitude should be close to 1;
   `gyro_dps` should be close to zero. Rotate each physical axis separately;
   verify axes respond independently and record orientation/signs.
3. Leave the board stationary for at least 30 seconds to settle. Run
   `imu calibrate`. Do not touch it for ~10 seconds. Accept only a successful
   result with 1000 samples. Motion, a bus error, >100 ms without fresh data or
   a >15 s calibration window cancels the attempt and preserves the old bias.
4. The acceptance gates are |a| in 0.9–1.1 g, each instantaneous gyro axis within
   +/-10 deg/s, per-axis accel standard deviation <=0.015 g, gyro standard
   deviation <=0.5 deg/s, and each mean gyro bias within +/-5 deg/s.
   These are initial engineering thresholds, not datasheet accuracy guarantees.
   Slow constant rotation, especially yaw, can pass a stationary detector:
   physically keep the sensor still. This is not six-position accelerometer calibration.
5. Run `imu save`, require `result=0`, then reset without reflashing. Require
   `Calibration load result=0 bias_valid=1`, and compare `imu status` bias values
   before/after reset (stored at 1 micro-degree/s resolution). Check corrected
   gyro readings remain close to zero. Replace the module -> recalibrate/clear.
6. Run `imu stream off` for a 10-minute soak, retaining the 10-second health lines.
   Target approximately 1000 fresh samples/10 s, no increasing I/O errors, no
   stalls or resets. `stale` means no new pair was available; `late` means the
   loop missed a deadline, not a measurement of all sensor-internal lost samples.
   Flash save/clear pauses are reported separately. Record actual counts rather
   than claiming 100 Hz from a compile-time constant.
7. With power disconnected, unplug the IMU and power up once to verify bounded
   initialization failure. Reconnect only with power off. Keep the console
   responsive and retain the failure log. Do not hot-unplug wires as a test.

Record firmware commit/hash, module/chip marking, wiring, address/WHO_AM_I,
measured cadence, calibration statistics, save/reboot evidence and 10-minute
logs in a PR/issue comment. Exclude Wi-Fi credentials, private paths and personal
identifiers from attachments. This application provides no pose fusion, gait,
fall detection, motor control or Wi-Fi dashboard yet.

## Sources

- [NUCODE NU-IMU product information](https://nucode.store/product/detail.html?product_no=37)
- [ST LSM6DS3 register definitions (ID 0x69)](https://github.com/STMicroelectronics/lsm6ds3-pid/blob/master/lsm6ds3_reg.h)
- [ST LSM6DS3TR-C datasheet (ID 0x6a)](https://www.st.com/resource/en/datasheet/lsm6ds3tr-c.pdf)
- [Zephyr LSM6DSL / LSM6DS3TR-C initialization report](https://github.com/zephyrproject-rtos/zephyr/issues/111240)
- [Pinned Ameba I2C driver](https://github.com/manjae-cho/zephyr/blob/e70694102ad6f910485155a824c5483daf605a9f/drivers/i2c/i2c_ameba.c)
