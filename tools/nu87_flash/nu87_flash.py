#!/usr/bin/env python3
"""Safely stage and invoke Realtek's AmebaD uploader for the NU-87."""

from __future__ import annotations

import argparse
import codecs
import hashlib
import os
from pathlib import Path
import platform
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request


FLASH_SIZE = 4 * 1024 * 1024
APP_OFFSET = 0x14000
KM4_BOOT_OFFSET = 0x4000
IMAGE2_OFFSET = 0x6000
ALLOWED_BAUDS = (115200, 921600, 1500000)
DEFAULT_BAUD = 115200

ARCHIVE_URL = (
    "https://raw.githubusercontent.com/Ameba-AIoT/ameba-arduino-d/"
    "d2865db726d651f086579c65853a76acbe6336e3/Arduino_package/release/"
    "ameba_d_tools_macos-1.1.3.tar.gz"
)
ARCHIVE_SHA256 = "fec90f971a7f22b4e6ffbe770d25da4bd52cbd92cc8d473620d8b86ac1040713"
LOADER_SHA256 = "9307121385cb390dfd2da64da2c6c515f17b5a9556b3d04021487c9b9f220b55"
UPLOADER_MEMBER = "ameba_d_tools_macos/upload_image_tool_macos"
LOADER_MEMBER = (
    "ameba_d_tools_macos/tools/macos/image_tool/"
    "imgtool_flashloader_amebad.bin"
)
SUCCESS_MARKER = "All images are sent successfully!"
MAX_ARCHIVE_SIZE = 16 * 1024 * 1024


class FlashError(RuntimeError):
    """A user-actionable staging or flashing error."""


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def validate_images(boot: bytes, app: bytes) -> None:
    if not boot:
        raise FlashError("bootloader_all.bin is empty")
    if not app:
        raise FlashError("km0_km4_app.bin is empty")
    if len(boot) > APP_OFFSET:
        raise FlashError(
            f"bootloader is {len(boot)} bytes; it overlaps the app at 0x{APP_OFFSET:x}"
        )
    if APP_OFFSET + len(app) > FLASH_SIZE:
        raise FlashError(
            f"application ends past the {FLASH_SIZE // (1024 * 1024)} MiB flash boundary"
        )


def repack_images(boot: bytes, app: bytes) -> tuple[bytes, bytes, bytes]:
    """Map Zephyr's two images into the vendor uploader's three fixed slots."""
    validate_images(boot, app)
    image_end = APP_OFFSET + len(app)
    flat = bytearray(b"\xff" * image_end)
    flat[: len(boot)] = boot
    flat[APP_OFFSET:image_end] = app
    return (
        bytes(flat[:KM4_BOOT_OFFSET]),
        bytes(flat[KM4_BOOT_OFFSET:IMAGE2_OFFSET]),
        bytes(flat[IMAGE2_OFFSET:]),
    )


def load_build_images(build_dir: Path) -> tuple[bytes, bytes]:
    image_dir = build_dir.expanduser().resolve() / "images"
    boot_path = image_dir / "bootloader_all.bin"
    app_path = image_dir / "km0_km4_app.bin"
    try:
        boot = boot_path.read_bytes()
        app = app_path.read_bytes()
    except OSError as exc:
        raise FlashError(f"cannot read Zephyr image: {exc}") from exc
    validate_images(boot, app)
    return boot, app


def validate_platform(system: str | None = None) -> None:
    if (system or platform.system()) != "Darwin":
        raise FlashError("this wrapper supports macOS (Darwin) only")


def validate_port(port: str, require_present: bool = False) -> None:
    if not (port.startswith("/dev/cu.") or port.startswith("/dev/tty.")):
        raise FlashError("port must be a macOS /dev/cu.* or /dev/tty.* device")
    if require_present:
        try:
            mode = os.stat(port).st_mode
        except OSError as exc:
            raise FlashError(f"serial port is unavailable: {port}: {exc}") from exc
        if not stat.S_ISCHR(mode):
            raise FlashError(f"serial port is not a character device: {port}")


def validate_baud(baud: int) -> None:
    if baud not in ALLOWED_BAUDS:
        allowed = ", ".join(str(value) for value in ALLOWED_BAUDS)
        raise FlashError(f"unsupported baud {baud}; choose one of: {allowed}")


def download_archive(destination: Path) -> None:
    request = urllib.request.Request(
        ARCHIVE_URL,
        headers={"User-Agent": "microWallaby/nu87-flash"},
    )
    total = 0
    try:
        with (
            urllib.request.urlopen(request, timeout=30) as response,
            destination.open("wb") as out,
        ):
            while True:
                block = response.read(64 * 1024)
                if not block:
                    break
                total += len(block)
                if total > MAX_ARCHIVE_SIZE:
                    raise FlashError("Realtek tool archive exceeds the safety size limit")
                out.write(block)
    except (OSError, urllib.error.URLError) as exc:
        raise FlashError(f"cannot download the pinned Realtek tool archive: {exc}") from exc


def verify_archive(archive: Path) -> None:
    try:
        actual = sha256_file(archive)
    except OSError as exc:
        raise FlashError(f"cannot read Realtek tool archive: {exc}") from exc
    if actual != ARCHIVE_SHA256:
        raise FlashError(
            "Realtek tool archive SHA-256 mismatch: "
            f"expected {ARCHIVE_SHA256}, got {actual}"
        )


def _read_regular_member(bundle: tarfile.TarFile, name: str) -> bytes:
    try:
        member = bundle.getmember(name)
    except KeyError as exc:
        raise FlashError(f"required archive member is missing: {name}") from exc
    if not member.isfile():
        raise FlashError(f"archive member is not a regular file: {name}")
    source = bundle.extractfile(member)
    if source is None:
        raise FlashError(f"cannot read archive member: {name}")
    return source.read()


def stage_bundle(
    archive: Path,
    destination: Path,
    vendor_images: tuple[bytes, bytes, bytes],
) -> Path:
    """Extract only known files, verify them, and add repacked images."""
    verify_archive(archive)
    try:
        with tarfile.open(archive, "r:gz") as source:
            uploader = _read_regular_member(source, UPLOADER_MEMBER)
            loader = _read_regular_member(source, LOADER_MEMBER)
    except (OSError, tarfile.TarError) as exc:
        raise FlashError(f"cannot inspect Realtek tool archive: {exc}") from exc

    loader_hash = sha256_bytes(loader)
    if loader_hash != LOADER_SHA256:
        raise FlashError(
            "Realtek flash loader SHA-256 mismatch: "
            f"expected {LOADER_SHA256}, got {loader_hash}"
        )

    destination.mkdir(parents=True, exist_ok=True)
    uploader_path = destination / "upload_image_tool_macos"
    uploader_path.write_bytes(uploader)
    uploader_path.chmod(0o700)
    (destination / "imgtool_flashloader_amebad.bin").write_bytes(loader)

    names = ("km0_boot_all.bin", "km4_boot_all.bin", "km0_km4_image2.bin")
    for name, content in zip(names, vendor_images, strict=True):
        (destination / name).write_bytes(content)
    return uploader_path


def build_uploader_command(
    bundle: Path,
    port: str,
    baud: int,
    machine: str | None = None,
) -> list[str]:
    validate_baud(baud)
    uploader = str(bundle / "upload_image_tool_macos")
    command = [
        uploader,
        str(bundle),
        port,
        "Ameba_AMB26",
        "Disable",
        "Disable",
        str(baud),
    ]
    architecture = (machine or platform.machine()).lower()
    if architecture in ("arm64", "aarch64"):
        return ["arch", "-x86_64", *command]
    if architecture in ("x86_64", "amd64"):
        return command
    raise FlashError(f"unsupported Mac architecture: {architecture}")


def upload_succeeded(returncode: int, output: str) -> bool:
    return returncode == 0 and SUCCESS_MARKER in output


def run_uploader(command: list[str]) -> tuple[int, str]:
    """Run the vendor process while mirroring and retaining its output."""
    try:
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
    except OSError as exc:
        raise FlashError(
            f"cannot start Realtek uploader (is Rosetta installed?): {exc}"
        ) from exc

    assert process.stdout is not None
    decoder = codecs.getincrementaldecoder("utf-8")("replace")
    captured: list[str] = []
    while True:
        block = process.stdout.read1(4096)
        if not block:
            break
        text = decoder.decode(block)
        captured.append(text)
        print(text, end="", flush=True)
    tail = decoder.decode(b"", final=True)
    if tail:
        captured.append(tail)
        print(tail, end="", flush=True)
    return process.wait(), "".join(captured)


def print_boot_instructions() -> None:
    print("NU-87 ROM download mode:")
    print("  1. Hold BOOT.")
    print("  2. Press and release RESET.")
    print("  3. Release BOOT.")
    print("Keep the USB cable connected; the uploader starts next.")


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--port", required=True)
    parser.add_argument(
        "--archive",
        type=Path,
        help="local ameba_d_tools_macos-1.1.3.tar.gz (otherwise download the pinned archive)",
    )
    parser.add_argument("--baud", type=int, choices=ALLOWED_BAUDS, default=DEFAULT_BAUD)
    parser.add_argument(
        "--write",
        action="store_true",
        help="open the serial port and write flash (default is validation-only dry-run)",
    )
    return parser


def execute(args: argparse.Namespace) -> int:
    validate_platform()
    validate_port(args.port, require_present=args.write)
    validate_baud(args.baud)
    boot, app = load_build_images(args.build_dir)
    vendor_images = repack_images(boot, app)

    print(f"bootloader: {len(boot)} bytes at 0x000000")
    print(f"application: {len(app)} bytes at 0x{APP_OFFSET:06x}")
    print(f"baud: {args.baud}")
    if not args.write:
        print("DRY RUN: images validated; the serial port was not opened.")
        print(f"Pinned Realtek archive: {ARCHIVE_URL}")
        print(f"Archive SHA-256: {ARCHIVE_SHA256}")
        print("Run the same command with --write only after checking the mapping above.")
        return 0

    with tempfile.TemporaryDirectory(prefix="nu87-flash-") as temporary:
        temporary_path = Path(temporary)
        if args.archive is None:
            archive = temporary_path / "ameba_d_tools_macos-1.1.3.tar.gz"
            print("Downloading pinned Realtek AmebaD uploader...")
            download_archive(archive)
        else:
            archive = args.archive.expanduser().resolve()

        bundle = temporary_path / "bundle"
        uploader = stage_bundle(archive, bundle, vendor_images)
        if not uploader.is_file():
            raise FlashError("staged uploader is missing")
        command = build_uploader_command(bundle, args.port, args.baud)
        if command[:2] == ["arch", "-x86_64"] and shutil.which("arch") is None:
            raise FlashError("macOS arch utility is unavailable")
        print_boot_instructions()
        returncode, output = run_uploader(command)
        if not upload_succeeded(returncode, output):
            raise FlashError(
                "flash was not confirmed: the uploader must exit 0 and print "
                f"{SUCCESS_MARKER!r}"
            )
    print("NU-87 flash completed and was confirmed by the uploader.")
    return 0


def main(argv: list[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    try:
        return execute(args)
    except FlashError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
