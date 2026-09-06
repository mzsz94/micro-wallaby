# NU-87 I/O bring-up application

This application is the first executable slice of the microWallaby Zephyr
architecture. It validates the NU-87 GPIO, ADC, periodic execution and onboard
RGB paths with the touch and joystick modules currently available. It never
produces a motor command.

## Runtime structure

| Unit | Rate | Responsibility |
| --- | --- | --- |
| `sensor_fast` | 100 Hz | Own GPIO/ADC, debounce digital inputs and publish the latest snapshot |
| `safety_ctrl` | 50 Hz | Check freshness, latch faults and enforce explicit fault clearing |
| `behavior_sm` | 20 Hz | Own `BOOT`, `SELF_TEST`, `SAFE_IDLE`, `MANUAL_EMU` and `FAULT` state |
| `indicator` | 50 Hz | Own RGB and apply a safety fault override before behavior state |
| `health_log` | 1 Hz | Print bounded telemetry and error/deadline counters |

The RGB owner maps green to `SAFE_IDLE`, blue to `MANUAL_EMU` and red to
`FAULT`. `MANUAL_EMU` only visualizes input intent; no actuator backend exists
in this application.

## Electrical limits

- Power both modules from `VDD33/3V3`, never from 5 V while connected directly
  to NU-87 inputs.
- Confirm the actual module silkscreen before wiring. Do not infer header order
  from a product photograph.
- Change wiring only while the board is unpowered.
- The joystick switch uses the NU-87 internal pull-up and 100 Hz polling. Do not
  enable its GPIO interrupt until an external pull-up is fitted and tested.
- The touch D0 polarity is not inferred at runtime. The overlay starts with
  `GPIO_ACTIVE_HIGH`; characterize the module, then explicitly change it to
  `GPIO_ACTIVE_LOW` if idle and touched levels are reversed.

## Fixture A: combined behavior test

| Module signal | NU-87 pin |
| --- | --- |
| Touch `D0` | `PA15` |
| Joystick `VRX` | `PB1 / ADC4` |
| Joystick `VRY` | `PB2 / ADC5` |
| Joystick `SW` | `PB22`, internal pull-up, active low |
| Touch `VCC` | `VDD33/3V3` |
| Joystick `5V`-labelled supply pin | `VDD33/3V3` (do not connect to 5 V) |
| Both `GND` | `GND` |

`D0` belongs to the touch module; the joystick has no `D0` pin. Leave touch
`A0` disconnected in this fixture. A debounced touch latches `FAULT`. The fault
can only be cleared after the touch is inactive and the joystick is neutral,
then a post-fault `SW` release followed by a new two-second press is observed.

For a formal run, first use the `raw_d0` log field to confirm D0 polarity and
restart after changing the overlay if needed. Keep the joystick centered for the
first five seconds, then leave touch and `SW` inactive until the five-minute
boot-idle window completes. Only then start intentional touch, switch and
joystick sweeps so they are not counted as idle false assertions.

## Fixture B: touch analog characterization

The available touch module exposes `VCC/GND/D0/A0`. Disconnect the joystick,
then connect touch `A0` to `PB1/ADC4` and `D0` to `PA15`. This profile logs the
A0 value and corresponding debounced D0 level; it is not a full safety
state-machine acceptance test.

## Reproducible Zephyr workspace

`west.yml` pins the still-open NU-87 board-support pull request to commit
`e70694102ad6f910485155a824c5483daf605a9f`. In a dedicated west workspace,
initialize this repository as the local manifest and run `west update`. Once the
NU-87 port is merged upstream, replace the staging fork with a reviewed upstream
Zephyr revision.

Build-only fixture A without the binary NP image:

```sh
west build -b nucode_nu87 micro-wallaby/firmware/nu87_io_bringup \
  -d build/nu87-fixture-a -- \
  -DDTC_OVERLAY_FILE=boards/nucode_nu87_fixture_a.overlay \
  -DEXTRA_CONF_FILE=fixture_a.conf
```

Build fixture B by selecting `nucode_nu87_fixture_b.overlay` and
`fixture_b.conf` instead.

A programmable image also needs the Realtek NP-core blob:

```sh
west blobs fetch hal_realtek

python -m pip install -r modules/hal/realtek/ameba/scripts/requirements.txt
python -m pip install 'python-mbedtls==2.10.1'

NU87_SDK=/Users/mzsz/zephyr-sdk-1.0.1
NU87_SDK_COMPAT="$(mktemp -d)"
ln -s "$NU87_SDK/gnu/arm-zephyr-eabi" \
  "$NU87_SDK_COMPAT/arm-zephyr-eabi"

ZEPHYR_TOOLCHAIN_VARIANT=zephyr \
ZEPHYR_SDK_INSTALL_DIR="$NU87_SDK_COMPAT" \
west build -b nucode_nu87 micro-wallaby/firmware/nu87_io_bringup \
  -d build/nu87-fixture-a -p always -- \
  -DZEPHYR_SDK_INSTALL_DIR="$NU87_SDK" \
  -DCMAKE_GDB="$NU87_SDK/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb" \
  -DDTC_OVERLAY_FILE=boards/nucode_nu87_fixture_a.overlay \
  -DEXTRA_CONF_FILE=fixture_a.conf \
  -DCONFIG_SOC_AMEBA_NP_IMAGE=y
```

The extra SDK path setup is a temporary compatibility workaround: the pinned
Realtek image merger expects the pre-1.0 SDK directory layout. Its security
module also imports `python-mbedtls` even for this non-secure image path,
although that package is no longer declared by the HAL requirements.

The current board port does not provide `west flash`. On macOS, validate the
generated images with the repository wrapper from the `micro-wallaby` root:

```sh
python3 tools/nu87_flash/nu87_flash.py \
  --build-dir ../build/nu87-fixture-a \
  --port /dev/cu.usbserial-10
```

Then put the board in ROM download mode (hold `BOOT`, press and release
`RESET`, then release `BOOT`) and repeat the command with `--write`. The first
hardware run defaults to the conservative 115,200 baud; 921,600 and 1,500,000
are available after that path is proven. See
[`tools/nu87_flash/README.md`](../../tools/nu87_flash/README.md) for details.

The wrapper preserves the board port's required flash map:

- `images/bootloader_all.bin` at `0x000000`
- `images/km0_km4_app.bin` at `0x014000`

## Tests

The filtering, axis normalization, fault latch and behavior transitions are
portable C and can be tested without Zephyr:

```sh
make -C firmware/nu87_io_bringup/tests/host test
```

The actual Zephyr build and hardware acceptance remain required because the
NU-87 board PR has not yet validated ADC or these external pins.
