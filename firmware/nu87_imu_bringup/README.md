# NU-87 IMU bring-up

Issue: [#17 — Implement the IMU driver and calibration procedure](https://github.com/mzsz94/micro-wallaby/issues/17).

The hardware target is the NUCODE NU-IMU-LSM6DS3TR module. This standalone Zephyr
application does not enable Wi-Fi, joystick, touch inputs or motors.

Read [the design and bench procedure](../../docs/imu-bringup.md) before wiring.
For a prebuilt image, use [the Mac flashing guide](../../docs/imu-macos-flashing.md).

## Build

Use the existing `west.yml` revisions and install the Zephyr / Realtek image-tool
requirements as described by the I/O bring-up guide. Zephyr SDK 1.0.1 was used.
From the repository root, with the west Python environment active:

```sh
tools/apply_west_patches.sh
west blobs fetch hal_realtek
python -m pip install -r ../modules/hal/realtek/ameba/scripts/requirements.txt
python -m pip install 'python-mbedtls==2.10.1'

IMU_SDK=/absolute/path/to/zephyr-sdk-1.0.1
IMU_SDK_COMPAT=$(mktemp -d)
ln -s "$IMU_SDK/gnu/arm-zephyr-eabi" "$IMU_SDK_COMPAT/arm-zephyr-eabi"
ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR="$IMU_SDK_COMPAT" \
west build -b nucode_nu87 firmware/nu87_imu_bringup \
  -d /tmp/micro-wallaby-imu-build -- \
  -DZEPHYR_SDK_INSTALL_DIR="$IMU_SDK" \
  -DCMAKE_GDB="$IMU_SDK/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb" \
  -DEXTRA_CONF_FILE=program.conf
```

The compatibility directory is required by the pinned Realtek image merger,
which expects the older SDK directory layout. Do not overwrite an existing build
directory belonging to a different application.

Default: `st,lsm6ds3`, WHO_AM_I `0x69`, I2C address `0x6a`, PA25 SCL / PA26 SDA.
After confirming hardware, use `-DEXTRA_DTC_OVERLAY_FILE=boards/address_6b.overlay`
for SA0 high. Only for an actual **LSM6DS3TR-C**, use
`-DEXTRA_DTC_OVERLAY_FILE=boards/lsm6ds3tr_c.overlay` (expected ID `0x6a`).
Both extra overlays can be passed as a quoted semicolon-separated list.
I2C address and WHO_AM_I are different values; do not confuse them.

## Test

```sh
make -C firmware/nu87_imu_bringup/tests/host test
```

Host tests use a fake register bus; they do not establish electrical correctness.
The local driver exposes the Zephyr Sensor API in SI units. The serial test UI
converts to g and degrees/second for easier bench interpretation.

## Package

Commit the source, then reconfigure/rebuild (`west build -c ...`) before packaging.
The packager rejects dirty source, a mismatched source revision, non-IMU builds,
network-enabled builds, missing full images, or images overlapping NVS storage.

```sh
python3 tools/nu87_imu/package.py \
  --build-dir /tmp/micro-wallaby-imu-build \
  --zephyr-dir ../zephyr --hal-dir ../modules/hal/realtek \
  --sdk-dir "$IMU_SDK" --output-dir /tmp/micro-wallaby-imu-package
```

The `NU-87 IMU flash bundle` workflow also builds an artifact for Mac flashing.
Artifacts require GitHub login and expire after 30 days; the source remains in Git.
