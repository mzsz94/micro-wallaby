#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate and optionally flash NU87 images with the pinned Linux AmebaD uploader.

Default is a dry run: enumerate USB identity, hash images and stage vendor files;
never open a serial port. Only --write invokes the uploader. Manual ROM entry is
the default; --auto-boot opts into the vendor's DTR/RTS download sequence on boards
that support it. The success marker is vendor confirmation, not an independent
flash readback or a running-firmware test.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import platform
import re
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
from contextlib import contextmanager, nullcontext
from pathlib import Path

USB_VID = 0x10C4
USB_PID = 0xEA60
APP_OFFSET = 0x14000
APP_LIMIT = 0x200000
STORAGE_OFFSET = 0x250000
KM4_BOOT_OFFSET = 0x4000
IMAGE2_OFFSET = 0x6000
# AmebaD 3.1.9 normal (non-Mesh) flash configuration reserves UART settings
# at 0xfc000, AP settings at 0xfe000 and FTL pages at 0x102000..0x104fff.
ARDUINO_APP_LIMIT = 0xFC000
ARDUINO_FTL_OFFSET = 0x102000
ARDUINO_FTL_END = 0x105000
ARDUINO_BOOT_SHA256 = (
    "453c880307fc389009aa8a39c63da3d715b207ad797206c896cc691426daaa64",
    "05fbf808d43113eaf7c11b75091c986b817135f0c52eb9fa94bb9fe9f34b062c",
)
ARDUINO_KM0_SIZE = 0x1B000
ARDUINO_KM0_SHA256 = "9595267fe00aa42d0f044c432689fc6cec4a72ba5262d03f0fe159be6d4131fe"
IMAGE2_MAGIC = b"81958711"
IMAGE_HEADER_SIZE = 32
DEFAULT_BAUD = 115200
ALLOWED_BAUDS = (115200, 921600, 1500000)
ARCHIVE_NAME = "ameba_d_tools_linux-1.1.3.tar.gz"
# Arduino's realtek package index specifies this archive's SHA; pin its commit too.
ARCHIVE_URL = (
    "https://raw.githubusercontent.com/Ameba-AIoT/ameba-arduino-d/"
    "d2865db726d651f086579c65853a76acbe6336e3/Arduino_package/release/"
    + ARCHIVE_NAME
)
ARCHIVE_SHA256 = "f3cb543d63c59ba3d66f1181252cec407824d5cc196cf2fb1719aee4328c2c0e"
UPLOADER_SHA256 = "d919636c0c8ed700b8553a243a7b7c150a7b6cd24399ba98e460e2539396004a"
LOADER_SHA256 = "9307121385cb390dfd2da64da2c6c515f17b5a9556b3d04021487c9b9f220b55"
UPLOADER_NAME = "upload_image_tool_linux"
LOADER_NAME = "imgtool_flashloader_amebad.bin"
UPLOADER_MEMBER = "ameba_d_tools_linux/" + UPLOADER_NAME
LOADER_MEMBER = "ameba_d_tools_linux/tools/linux/image_tool/" + LOADER_NAME
VENDOR_IMAGE_NAMES = ("km0_boot_all.bin", "km4_boot_all.bin", "km0_km4_image2.bin")
SUCCESS_MARKER = "All images are sent successfully!"
MAX_ARCHIVE_SIZE = 16 * 1024 * 1024


class FlashError(RuntimeError):
    """Validation or upload was not confirmed."""


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def validate_platform() -> None:
    if platform.system() != "Linux" or platform.machine().lower() not in ("x86_64", "amd64"):
        raise FlashError("the pinned uploader requires x86_64 Linux (including WSL)")


def validate_images(boot: bytes, app: bytes) -> None:
    if not boot or not app:
        raise FlashError("bootloader_all.bin and km0_km4_app.bin must both be nonempty")
    if len(boot) > APP_OFFSET:
        raise FlashError("bootloader overlaps application at 0x14000")
    if APP_OFFSET + len(app) > APP_LIMIT:
        raise FlashError("application crosses the protected 0x200000 boundary")


def load_images(build_dir: Path) -> tuple[bytes, bytes]:
    directory = build_dir.expanduser().resolve() / "images"
    try:
        boot = (directory / "bootloader_all.bin").read_bytes()
        app = (directory / "km0_km4_app.bin").read_bytes()
    except OSError as exc:
        raise FlashError(f"cannot read build/images: {exc}") from exc
    validate_images(boot, app)
    return boot, app


def repack_images(boot: bytes, app: bytes) -> tuple[bytes, bytes, bytes]:
    """Preserve Zephyr offsets in the vendor's 0/0x4000/0x6000 fixed slots."""
    validate_images(boot, app)
    flat = bytearray(b"\xff" * (APP_OFFSET + len(app)))
    flat[:len(boot)] = boot
    flat[APP_OFFSET:] = app
    return bytes(flat[:KM4_BOOT_OFFSET]), bytes(flat[KM4_BOOT_OFFSET:IMAGE2_OFFSET]), \
        bytes(flat[IMAGE2_OFFSET:])


def _arduino_segment_end(data: bytes, offset: int, address: int, *, empty=False) -> int:
    """Validate one official pick-tool image header, not a payload checksum."""
    if offset + IMAGE_HEADER_SIZE > len(data):
        raise FlashError("Arduino image2 has a truncated segment header")
    header = data[offset:offset + IMAGE_HEADER_SIZE]
    length, load_address = struct.unpack_from("<II", header, 8)
    if (header[:8] != IMAGE2_MAGIC or load_address != address
            or header[16:] != b"\xff" * 16):
        raise FlashError(f"Arduino image2 has an unexpected header at 0x{offset:x}")
    end = offset + IMAGE_HEADER_SIZE + length
    if (not empty and length == 0) or end > len(data):
        raise FlashError("Arduino image2 has an empty or truncated segment payload")
    if ((address == 0x10005000 and address + length > 0x1007C000)
            or (address == 0x02000020 and address + length > 0x02400000)):
        raise FlashError("Arduino image2 segment exceeds the supported RAM region")
    return end


def _arduino_padding(data: bytes, offset: int) -> int:
    end = (offset + 4095) & ~4095
    if end > len(data) or data[offset:end] != b"\xff" * (end - offset):
        raise FlashError("Arduino image2 has missing or unexpected 4KB padding")
    return end


def validate_arduino_images(images: tuple[bytes, bytes, bytes]) -> None:
    """Accept the Arduino 3.1.9 AMB26 PMU/NONE layout, without repacking it."""
    boot0, boot4, app = images
    for name, data, limit, digest in zip(
            VENDOR_IMAGE_NAMES[:2], (boot0, boot4),
            (KM4_BOOT_OFFSET, IMAGE2_OFFSET - KM4_BOOT_OFFSET),
            ARDUINO_BOOT_SHA256, strict=True):
        if not data or len(data) > limit:
            raise FlashError(f"Arduino {name} is empty or overlaps the next vendor slot")
        if sha256(data) != digest:
            raise FlashError(f"Arduino {name} is not the pinned 3.1.9 PMU/NONE boot image")
    if not app or IMAGE2_OFFSET + len(app) > ARDUINO_APP_LIMIT:
        raise FlashError("Arduino image2 is empty or crosses reserved settings at 0xfc000")
    # Official postbuild concatenates the 4KB-padded KM0 image and the padded
    # KM4 XIP/RAM/PSRAM image. Read lengths at known boundaries, never scan for
    # magic bytes inside executable payloads.
    end = _arduino_segment_end(app, 0, 0x0C000020)
    end = _arduino_segment_end(app, end, 0x00083000)
    end = _arduino_padding(app, end)
    if end != ARDUINO_KM0_SIZE or sha256(app[:end]) != ARDUINO_KM0_SHA256:
        raise FlashError("Arduino image2 does not contain the pinned 3.1.9 KM0 image")
    end = _arduino_segment_end(app, end, 0x0E000020)
    end = _arduino_segment_end(app, end, 0x10005000)
    end = _arduino_segment_end(app, end, 0x02000020, empty=True)
    if _arduino_padding(app, end) != len(app):
        raise FlashError("Arduino image2 contains trailing data outside its five segments")


def load_arduino_images(build_dir: Path) -> tuple[bytes, bytes, bytes]:
    directory = build_dir.expanduser().resolve() / "images"
    try:
        images = tuple((directory / name).read_bytes() for name in VENDOR_IMAGE_NAMES)
    except OSError as exc:
        raise FlashError(f"cannot read Arduino build/images: {exc}") from exc
    validate_arduino_images(images)
    return images


def validate_identity(port: str, serial_number: str, ports=None, require_device=False):
    """Enumerate only; CP210x identity is not proof of the attached MCU model."""
    if not serial_number or not serial_number.strip():
        raise FlashError("an exact, nonempty USB --serial-number is required")
    if not re.fullmatch(r"/dev/ttyUSB[0-9]+", port):
        raise FlashError("--port must name a Linux /dev/ttyUSB<number> device")
    if ports is None:
        from serial.tools import list_ports
        ports = list_ports.comports()
    matches = [entry for entry in ports
               if entry.device == port and entry.vid == USB_VID and entry.pid == USB_PID
               and entry.serial_number == serial_number]
    if len(matches) != 1:
        raise FlashError(f"{port} does not uniquely match 10c4:ea60 serial {serial_number!r}")
    if require_device:
        try:
            mode = os.stat(port).st_mode
        except OSError as exc:
            raise FlashError(f"serial device unavailable: {exc}") from exc
        if not stat.S_ISCHR(mode):
            raise FlashError("selected serial port is not a character device")
    return matches[0]


def download_archive(destination: Path) -> None:
    request = urllib.request.Request(ARCHIVE_URL, headers={"User-Agent": "NU87-linux-flash"})
    try:
        with urllib.request.urlopen(request, timeout=30) as source, destination.open("wb") as out:
            total = 0
            while block := source.read(65536):
                total += len(block)
                if total > MAX_ARCHIVE_SIZE:
                    raise FlashError("vendor archive exceeds size limit")
                out.write(block)
    except (OSError, urllib.error.URLError) as exc:
        raise FlashError(f"cannot download pinned vendor archive: {exc}") from exc


def select_archive(requested: Path | None, temporary: Path, allow_download: bool) -> Path:
    if requested is not None:
        return requested.expanduser().resolve()
    cached = Path.home() / ".arduino15/staging/packages" / ARCHIVE_NAME
    if cached.is_file():
        return cached
    if not allow_download:
        raise FlashError("vendor archive is not cached; provide --archive for an offline dry run")
    downloaded = temporary / ARCHIVE_NAME
    download_archive(downloaded)
    return downloaded


def read_member(source: tarfile.TarFile, name: str) -> bytes:
    matches = [member for member in source.getmembers() if member.name == name]
    if len(matches) != 1 or not matches[0].isfile():
        raise FlashError(f"archive requires one regular file: {name}")
    if matches[0].size > MAX_ARCHIVE_SIZE:
        raise FlashError(f"archive member too large: {name}")
    stream = source.extractfile(matches[0])
    if stream is None:
        raise FlashError(f"unreadable archive member: {name}")
    with stream:
        return stream.read()


def stage_bundle(archive: Path, destination: Path, images: tuple[bytes, bytes, bytes]) -> Path:
    """Copy just the known tool, loader and exact generated images; no settings."""
    try:
        if archive.stat().st_size > MAX_ARCHIVE_SIZE:
            raise FlashError("vendor archive exceeds size limit")
        if sha256(archive.read_bytes()) != ARCHIVE_SHA256:
            raise FlashError("vendor archive SHA-256 mismatch")
        with tarfile.open(archive, "r:gz") as source:
            uploader = read_member(source, UPLOADER_MEMBER)
            loader = read_member(source, LOADER_MEMBER)
    except (OSError, tarfile.TarError) as exc:
        raise FlashError(f"cannot read vendor archive: {exc}") from exc
    if sha256(uploader) != UPLOADER_SHA256 or sha256(loader) != LOADER_SHA256:
        raise FlashError("vendor uploader or AmebaD loader SHA-256 mismatch")
    # A new directory excludes stale settings, OTP, erase or image selections.
    destination.mkdir(mode=0o700)
    uploader_path = destination / UPLOADER_NAME
    uploader_path.write_bytes(uploader)
    uploader_path.chmod(0o700)
    (destination / LOADER_NAME).write_bytes(loader)
    for name, image in zip(VENDOR_IMAGE_NAMES, images, strict=True):
        (destination / name).write_bytes(image)
    verify_staged_images(destination, images)
    return uploader_path


def verify_staged_images(bundle: Path, images: tuple[bytes, bytes, bytes]) -> None:
    for name, expected in zip(VENDOR_IMAGE_NAMES, images, strict=True):
        if (bundle / name).read_bytes() != expected:
            raise FlashError(f"staged image changed: {name}")


def uploader_command(bundle: Path, port: str, baud: int, *, auto_boot: bool = False) -> list[str]:
    if baud not in ALLOWED_BAUDS:
        raise FlashError("unsupported upload baud")
    # Official platform.txt: bundle, port, board, auto_mode, erase_flash, baud.
    return [str(bundle / UPLOADER_NAME), str(bundle), port, "Ameba_AMB26",
            "Enable" if auto_boot else "Disable", "Disable", str(baud)]


@contextmanager
def inactive_modem_lines(port: str):
    """Hold a non-reading handle with DTR/RTS inactive during the native upload.

    Set both before open. The extra open handle keeps the vendor's subsequent
    open from being a first open; auto upload is separately disabled in its CLI.
    """
    import serial
    handle = serial.Serial(port=None, baudrate=DEFAULT_BAUD, timeout=0, write_timeout=1,
                           rtscts=False, dsrdtr=False, xonxoff=False)
    try:
        handle.dtr = False
        handle.rts = False
        handle.port = port
        handle.open()
        yield handle
    except serial.SerialException as exc:
        raise FlashError(f"cannot hold DTR/RTS inactive: {exc}") from exc
    finally:
        handle.close()


def upload_succeeded(returncode: int, output: str) -> bool:
    # Worker failures can otherwise leave a process with returncode 0.
    failures = ("handshake fail", "download fail", "checksum error", "erase flash fail",
                "images download fail", "clear flash status register fail", "error:")
    return (returncode == 0 and SUCCESS_MARKER in output
            and not any(message in output.lower() for message in failures))


def wait_for_manual_boot(input_fn=None) -> None:
    """Request the hardware sequence after the guard's first serial open."""
    if input_fn is None:
        input_fn = input
    print("DTR/RTS=False로 포트를 열었습니다. 이제 NU87을 수동 ROM 모드로 진입시키세요.",
          flush=True)
    print("BOOT를 누른 채 RESET을 눌렀다 놓고, BOOT를 놓으세요.", flush=True)
    try:
        input_fn("위 조작을 마쳤으면 Enter를 누르세요: ")
    except (EOFError, KeyboardInterrupt) as exc:
        raise FlashError("manual ROM entry was not completed; uploader was not started") from exc


def run_uploader(command: list[str], bundle: Path, timeout: int = 180) -> tuple[int, str]:
    # Never search inherited loader paths or load arbitrary LD_PRELOAD hooks.
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith("LD_")}
    process = None
    try:
        process = subprocess.Popen(command, cwd=bundle, env=environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, encoding="utf-8", errors="replace")
        output, _ = process.communicate(timeout=timeout)
    except (subprocess.TimeoutExpired, KeyboardInterrupt):
        if process is not None:
            process.kill()
            output, _ = process.communicate()
            print(output, end="", flush=True)
        raise FlashError("uploader interrupted or timed out; flash state is unconfirmed") from None
    except OSError as exc:
        raise FlashError(f"cannot run pinned Linux uploader: {exc}") from exc
    print(output, end="", flush=True)
    return process.returncode, output


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--image-format", choices=("zephyr", "arduino"), default="zephyr",
                        help="Zephyr two-image repack, or Arduino 3.1.9 AMB26 three images")
    parser.add_argument("--port", required=True)
    parser.add_argument("--serial-number", required=True, help="exact CP210x USB serial number")
    parser.add_argument("--archive", type=Path,
                        help="pinned Linux 1.1.3 archive; cache used by default")
    parser.add_argument("--baud", type=int, choices=ALLOWED_BAUDS, default=DEFAULT_BAUD)
    parser.add_argument("--write", action="store_true", help="write flash; default only validates")
    parser.add_argument("--auto-boot", action="store_true",
                        help="use vendor DTR/RTS ROM entry; requires board wiring support")
    return parser


def execute(args: argparse.Namespace) -> int:
    validate_platform()
    if args.image_format == "arduino":
        images = load_arduino_images(args.build_dir)
        source_images = zip(VENDOR_IMAGE_NAMES, (0, KM4_BOOT_OFFSET, IMAGE2_OFFSET),
                            images, strict=True)
    else:
        boot, app = load_images(args.build_dir)
        images = repack_images(boot, app)
        source_images = (("bootloader_all.bin", 0, boot),
                         ("km0_km4_app.bin", APP_OFFSET, app))
    identity = validate_identity(args.port, args.serial_number, require_device=args.write)
    print(f"USB: {identity.device} 10c4:ea60 serial={identity.serial_number}")
    print("USB bridge identity alone does not identify its attached MCU.")
    print(f"Image format: {args.image_format}")
    for name, offset, data in source_images:
        print(f"{name}: offset=0x{offset:06x} bytes={len(data)} sha256={sha256(data)}")
    if args.image_format == "arduino":
        print(f"Write limit 0x{ARDUINO_APP_LIMIT:x}; settings and Zephyr NVS excluded.")
        print(f"Arduino BLE FTL may erase/write ordinary flash at "
              f"0x{ARDUINO_FTL_OFFSET:x}..0x{ARDUINO_FTL_END - 1:x} when firmware runs.")
        print("Arduino replaces Zephyr boot/application; reflash both Zephyr images to restore.")
    else:
        print(f"Write limit 0x{APP_LIMIT:x}; NVS at 0x{STORAGE_OFFSET:x} excluded.")
    auto_mode = "Enable" if args.auto_boot else "Disable"
    print(f"Vendor auto upload={auto_mode}; whole-chip erase=Disable; OTP operations=none.")
    with tempfile.TemporaryDirectory(prefix="nu87-flash-") as temporary:
        temporary = Path(temporary)
        archive = select_archive(args.archive, temporary, allow_download=args.write)
        bundle = temporary / "bundle"
        stage_bundle(archive, bundle, images)
        for name, image in zip(VENDOR_IMAGE_NAMES, images, strict=True):
            print(f"staged {name}: bytes={len(image)} sha256={sha256(image)}")
        print(f"Vendor archive SHA-256: {ARCHIVE_SHA256}")
        if not args.write:
            print("DRY RUN: files and USB identity validated; serial port was not opened.")
            return 0
        guard = nullcontext() if args.auto_boot else inactive_modem_lines(args.port)
        with guard:
            if args.auto_boot:
                print("Requesting vendor automatic ROM entry; hardware support is unverified.",
                      flush=True)
            else:
                wait_for_manual_boot()
            validate_identity(args.port, args.serial_number, require_device=True)
            verify_staged_images(bundle, images)
            command = uploader_command(bundle, args.port, args.baud, auto_boot=args.auto_boot)
            code, output = run_uploader(command, bundle)
        if not upload_succeeded(code, output):
            raise FlashError(
                "upload unconfirmed: require exit 0, vendor success marker and no errors")
    print("Vendor confirmed upload. Independent flash readback and firmware operation unverified.")
    if args.auto_boot:
        print("Observe the application's boot and DHCP logs separately; upload is not a link test.")
    else:
        print("BOOT를 누르지 말고 RESET을 한 번 눌러 앱을 시작하세요.", flush=True)
    return 0


def main(argv: list[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    try:
        return execute(args)
    except (FlashError, OSError, ImportError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
