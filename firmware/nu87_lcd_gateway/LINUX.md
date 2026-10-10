# NU87 Wi-Fi / NU40 BLE gateway on Linux and WSL

The PC must run the gateway application on NU87 to reach it over Wi-Fi. The
Arduino BLE-only face demo does not connect to Wi-Fi or expose the HTTP API.
The NU40 KIT display service is compatible with this gateway; microphone, alarm
and SMP availability depend on the firmware actually installed on NU40.

## Build

From the repository root, prepare the pinned workspace and crypto modules:

```bash
bash firmware/nu87_imu_bringup/setup.sh
bash firmware/nu87_lcd_gateway/setup.sh
```

The build script expects `.zephyr-sdk` to contain Zephyr SDK 1.0.1 with the GNU
ARM toolchain. It can be a local symlink to an installed SDK. This path, the
workspace and all generated firmware stay outside Git.

Configure the same network used by the Host PC. The password prompt is hidden:

```bash
python3 firmware/nu87_lcd_gateway/configure.py \
  --ssid 'YOUR_TEST_NETWORK' --security wpa2 --test-network
```

Select `CONFIG_GATEWAY_NU40_KIT=y` in the private `local.conf`. Leave the peer
address empty until the actual NU40 has been discovered and selected. The
existing configuration helper preserves the API token when reconfiguring Wi-Fi.

```bash
bash firmware/nu87_lcd_gateway/build.sh
```

`program.conf` includes the Realtek radio images. Omitting it can produce an
image that builds but cannot initialize the radio. The pinned workspace also
includes the HAL fixes needed by this application; an unrelated global Zephyr
checkout is not an equivalent build environment.

## Record the firmware

Use an environment with `pyserial` installed. First enumerate the USB bridge:

```bash
python3 -m serial.tools.list_ports -v
```

Supply its exact USB serial number. Without `--write`, this only validates the
images, USB identity and pinned vendor tools; it does not open the serial port.

```bash
python3 firmware/nu87_lcd_gateway/flash_linux.py \
  --build-dir build/nu87-lcd-gateway \
  --port /dev/ttyUSB0 --serial-number YOUR_USB_SERIAL
```

Add `--write` to record the firmware. At the prompt, hold BOOT, press and release
RESET, release BOOT, then press Enter. After successful upload, press RESET
without BOOT to start the application. The wrapper maps the Zephyr images to
their original flash offsets, excludes calibration storage and verifies the
vendor success marker.

`--auto-boot --write` instead requests Realtek's automatic DTR/RTS ROM-entry
sequence. This is an opt-in attempt and requires compatible board wiring; it is
not a guarantee that the NU87 can enter download mode without the buttons. The
vendor process has a bounded timeout. Whole-chip erase remains disabled.
The argument order follows [Realtek's uploader documentation](https://forum.amebaiot.com/t/imagetool-for-linux/1376).

## Observe Wi-Fi startup and connect the PC

```bash
python3 firmware/nu87_lcd_gateway/connect.py \
  --serial-number YOUR_USB_SERIAL --timeout 60
```

Opening a USB serial adapter can restart the board. Prefer HTTP checks after
Wi-Fi is already running. If startup prints the complete empty-eFuse warning,
`--acknowledge-efuse-warning` permits one newline to dismiss that warning; it
does not write eFuse. The observer redacts the configured password and API token
and saves the observed private DHCP address in
`build/nu87-lcd-gateway/gateway-connection.json`.

Start the existing Host PC interface using the observed IP:

```bash
python3 -m voice_tactical.server --no-browser \
  --gateway 192.168.1.10:8080 \
  --gateway-config firmware/nu87_lcd_gateway/local.conf
```

Select the NU40 in the dashboard's peer list. A BLE connection alone does not
confirm a display update: check the matching display ACK as well. See
[VOICE-TACTICAL.md](VOICE-TACTICAL.md) for optional audio, warning and update
services. Model installation is separate from this hardware link.

## Checks without hardware

```bash
python3 -m unittest discover -s firmware/nu87_lcd_gateway/tests -p 'test_*.py'
sh firmware/nu87_lcd_gateway/tests/run_kit_tests.sh
python3 -m unittest voice_tactical.tests.test_gateway
```

The Linux uploader tests cover flash bounds, both image layouts, pinned archive
and loader validation, exact USB identity, manual and automatic download modes,
dry runs that never open a port, timeout handling and vendor success detection.
The connection tests cover Linux/macOS selection, split log lines, credential
redaction, bounded observation and the one-byte warning acknowledgement.
