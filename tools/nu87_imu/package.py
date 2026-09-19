#!/usr/bin/env python3
"""Create an allowlisted, credential-free NU-87 IMU flashing bundle."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def git_revision(path: Path) -> str:
    return subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()


def validate_config(config: str) -> None:
    for symbol in ("CONFIG_SENSOR", "CONFIG_I2C", "CONFIG_NVS", "CONFIG_SOC_AMEBA_NP_IMAGE"):
        if f"{symbol}=y" not in config.splitlines():
            raise ValueError(f"Required flashable-IMU setting missing: {symbol}")
    for symbol in ("CONFIG_WIFI", "CONFIG_NETWORKING", "CONFIG_BUILD_ONLY_NO_BLOBS"):
        if f"{symbol}=y" in config.splitlines():
            raise ValueError(f"Not a standalone flashable IMU build: {symbol}")


def validate_payload(payload: dict[str, bytes]) -> None:
    for name, data in payload.items():
        if not name.startswith("licenses/") and re.search(
            rb"/Users/[^\s\x00]+|[A-Za-z0-9._%+-]+@gmail\.com", data
        ):
            raise ValueError(f"Potential personal path/email in allowlisted file: {name}")
        if name.startswith("/") or ".." in Path(name).parts:
            raise ValueError("Unsafe archive member")


def create_bundle(build: Path, zephyr: Path, hal: Path, sdk: Path, output: Path) -> Path:
    status = subprocess.check_output(
        ["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=all"], text=True
    )
    if status:
        raise ValueError("Commit/stash source changes before publishing a bundle")
    revision = git_revision(ROOT)
    if (build / "imu-source-revision.txt").read_text().strip() != revision:
        raise ValueError("Build revision differs from source; reconfigure and rebuild after committing")
    validate_config((build / "zephyr/.config").read_text())
    dts = (build / "zephyr/zephyr.dts").read_text()
    if 'compatible = "st,lsm6ds3"' not in dts or "imu@6a" not in dts:
        raise ValueError("Default NU-IMU bundle requires the LSM6DS3/0x6a overlay")
    # Do not package a 0x6b overlay under a misleading default-configuration name.
    node = dts.split("imu@6a", 1)[1].split("};", 1)[0]
    if not re.search(r"reg\s*=\s*<\s*0x6a\s*>", node):
        raise ValueError("The bundle's expected I2C address is 0x6a")

    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("nu87_flash", ROOT / "tools/nu87_flash/nu87_flash.py")
    flash = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(flash)
    boot, app = flash.load_build_images(build)
    if 0x14000 + len(app) > 0x250000:
        raise ValueError("Image overlaps the reserved calibration partition")

    payload = {
        "images/bootloader_all.bin": boot,
        "images/km0_km4_app.bin": app,
        "nu87_flash.py": (ROOT / "tools/nu87_flash/nu87_flash.py").read_bytes(),
        "README.md": (ROOT / "docs/imu-macos-flashing.md").read_bytes(),
        "imu-bringup.md": (ROOT / "docs/imu-bringup.md").read_bytes(),
        "licenses/zephyr-APACHE-2.0.txt": (zephyr / "LICENSE").read_bytes(),
        "licenses/realtek-APACHE-2.0.txt": (hal / "LICENSE").read_bytes(),
    }
    license_dir = sdk / "gnu/arm-zephyr-eabi/share/licenses"
    for relative in ("gcc/COPYING.RUNTIME", "gcc/COPYING3", "gcc/COPYING3.LIB",
                     "picolibc/COPYING.picolibc", "picolibc/COPYING.NEWLIB"):
        payload[f"licenses/{relative}"] = (license_dir / relative).read_bytes()
    for name, path in (("Zephyr", zephyr), ("Realtek", hal)):
        if (path / "NOTICE").is_file():
            payload[f"licenses/{name}-NOTICE"] = (path / "NOTICE").read_bytes()
    payload["BUILD.json"] = (json.dumps({
        "repository": "https://github.com/mzsz94/micro-wallaby",
        "commit": revision,
        "source": f"https://github.com/mzsz94/micro-wallaby/tree/{revision}",
        "zephyr_commit": git_revision(zephyr),
        "hal_realtek_commit": git_revision(hal),
        "patches": "Source repository patches/ (applied by tools/apply_west_patches.sh)",
        "board": "nucode_nu87", "module": "NU-IMU-LSM6DS3TR",
        "i2c_address": "0x6a", "expected_who_am_i": "0x69",
        "scl": "PA25", "sda": "PA26", "console_baud": 1500000,
        "sensor_odr_hz": 104, "poll_target_hz": 100,
        "hardware_verification": "pending; not performed by this build",
        "wifi_enabled": False,
    }, indent=2, sort_keys=True) + "\n").encode()
    payload["NOTICE.txt"] = (
        "microWallaby NU-87 IMU experiment. Source and revision: BUILD.json.\n"
        "Contains Zephyr, Realtek AmebaD HAL and prebuilt boot/KM0 firmware,\n"
        "Arm CMSIS interface code, and Zephyr SDK libgcc/picolibc runtime code.\n"
        "Upstream license texts are included in licenses/.\n"
        "Local changes include the IMU application and source-repository patches.\n"
        "The Realtek macOS uploader is not bundled; the wrapper downloads a pinned archive.\n"
        "No hardware validation or fitness for robot motion is asserted.\n"
    ).encode()
    validate_payload(payload)
    payload["SHA256SUMS"] = "".join(
        f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in sorted(payload.items())
    ).encode()
    output.mkdir(parents=True, exist_ok=True)
    archive = output / f"nu87-imu-{revision[:12]}.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
        for name, data in sorted(payload.items()):
            entry = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o100644 << 16
            bundle.writestr(entry, data)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix(".zip.sha256").write_text(f"{digest}  {archive.name}\n")
    return archive


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("build-dir", "zephyr-dir", "hal-dir", "sdk-dir", "output-dir"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    args = parser.parse_args()
    print(create_bundle(args.build_dir, args.zephyr_dir, args.hal_dir, args.sdk_dir, args.output_dir))


if __name__ == "__main__":
    main()
