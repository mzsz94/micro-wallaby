# Flash the NU-IMU experiment from a Mac

This is an experimental NU-87 image, not a completed hardware qualification.
Default hardware: **NUCODE NU-IMU-LSM6DS3TR**, ID 0x69, address 0x6a.
Check the wiring guide before applying power. Never treat I2C address 0x6a
as evidence that the chip is the `-C` variant (which also has ID 0x6a).

## 1. Download and verify

In GitHub Actions, open the successful **NU-87 IMU flash bundle** run and download
its `nu87-imu-macos` artifact. GitHub login is required. Extract the outer
artifact ZIP, then the inner `nu87-imu-<commit>.zip`. The artifact expires after
30 days; rerun the build when a fresh copy is needed. Source code stays in Git.

Open Terminal inside the extracted inner package:

```sh
shasum -a 256 -c SHA256SUMS
python3 --version
ls /dev/cu.*
```

Use Python 3.10 or newer. No Zephyr SDK, C compiler, west workspace, Wi-Fi password
or game ROM is needed on this Mac. On Apple Silicon, Realtek's x86_64 uploader
requires Rosetta; install it through macOS if prompted and accept Apple's terms
yourself. The Python wrapper itself is not tied to ARM64.

## 2. Flash

Close any `screen`/miniterm sessions using the board. Replace the example serial
port with the one found on **this Mac**; it can differ from the Mac mini's port.

```sh
python3 nu87_flash.py --build-dir . --port /dev/cu.usbserial-10
```

This is a dry run and does not open the port. It validates image bounds.
The package contains the bootloader at 0x000000 and KM0+KM4 app at 0x014000.
Flashing replaces the current joystick/NES firmware; keep its previous package
if you want to restore it. The reserved NVS partition is outside these images.

Hold **BOOT**, press/release **RESET**, then release **BOOT**. Run:

```sh
python3 nu87_flash.py --build-dir . --port /dev/cu.usbserial-10 --write
```

The wrapper fetches a SHA-256-pinned Realtek uploader archive; internet access is
needed for that first step. Alternatively supply the matching offline archive
with `--archive /path/to/ameba_d_tools_macos-1.1.3.tar.gz`.
Wait for `All images are sent successfully!`, then press RESET with BOOT released.

## 3. Read samples and calibrate

```sh
python3 -m venv .venv
.venv/bin/python -m pip install 'pyserial==3.5'
.venv/bin/python -m serial.tools.miniterm /dev/cu.usbserial-10 1500000 --raw
```

Press RESET once after opening the terminal to capture startup diagnostics.
Quit miniterm with **Ctrl+]**. macOS `screen` may not accept 1500000 baud; use
miniterm rather than changing the firmware baud rate.

After successful initialization, run these commands in the serial shell:

```text
imu status
imu stream off
imu calibrate
```

Keep the module still for ~10 seconds. Wait for `Calibration accepted`, then:

```text
imu save
imu status
```

Require save result 0. Press RESET and confirm the saved bias loads successfully.
Use `imu stream on` for live six-axis logs; `imu clear` removes saved and RAM bias.
See `imu-bringup.md` in this package for the full axis/soak/failure test checklist.

Report logs containing `WHO_AM_I`, `init`, `Calibration`, and `health`. Hardware
pass/fail remains pending until those measurements have actually been observed.
