#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build the pinned NU87 Arduino demo using workspace tools/cache; never flash.

The vendor postbuild program writes into its tools directory. This helper copies
those tools into build/, preserves the SDK linker flags, appends the two OTP write
guards, and snapshots validated images after a successful compile. It does not
install/update packages or invoke an upload command.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

from nu87_flash import (
    VENDOR_IMAGE_NAMES,
    FlashError,
    load_arduino_images,
    sha256,
    validate_arduino_images,
)

WORKSPACE = Path(__file__).resolve().parent.parent
SDK_VERSION = "3.1.9"
TOOLS_VERSION = "1.1.3"
FQBN = "realtek:AmebaD:Ameba_AMB26"
OTP_FLAGS = "-Wl,--wrap=EFUSEWrite8 -Wl,--wrap=EFUSE_LogicalMap_Write"
TOOL_PROGRAMS = ("postbuild_img2_arduino_linux", "amebasize",
                 "tools/linux/pick", "tools/linux/pad")
MANIFEST_NAME = "manifest.json"


class BuildError(RuntimeError):
    """The pinned build could not be prepared or validated."""


def workspace_output(path: Path) -> Path:
    result = path.expanduser().resolve()
    if not result.is_relative_to(WORKSPACE / "build") or result == WORKSPACE / "build":
        raise BuildError("build, tools and cache directories must be below workspace/build")
    # The helper uses argv lists, but the vendor postbuild internally uses system()
    # and has an unquoted cp hook. Refuse paths its recipes cannot safely handle.
    if not re.fullmatch(r"[A-Za-z0-9_./:+-]+", str(result)):
        raise BuildError("vendor postbuild requires paths without spaces or shell metacharacters")
    return result


def sdk_flags(data_dir: Path) -> tuple[Path, str]:
    platforms = data_dir / "packages/realtek/hardware/AmebaD"
    installed = sorted(path.name for path in platforms.iterdir()
                       if path.is_dir() and (path / "platform.txt").is_file())
    if installed != [SDK_VERSION]:
        raise BuildError(f"require only AmebaD {SDK_VERSION} installed; found {installed}")
    platform = platforms / SDK_VERSION / "platform.txt"
    matches = [line.split("=", 1)[1] for line in platform.read_text().splitlines()
               if line.startswith("compiler.c.elf.extra_flags=")]
    if len(matches) != 1 or not matches[0].strip():
        raise BuildError("platform.txt must define one nonempty compiler.c.elf.extra_flags")
    return platform, matches[0].strip() + " " + OTP_FLAGS


def stage_tools(source: Path, destination: Path) -> None:
    if not source.is_dir():
        raise BuildError(f"installed AmebaD tools {TOOLS_VERSION} not found: {source}")
    if not destination.exists():
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source, destination)
    if any(path.is_symlink() for path in destination.rglob("*")):
        raise BuildError("staged tools must not contain symbolic links")
    for name in TOOL_PROGRAMS:
        original, staged = source / name, destination / name
        if (not original.is_file() or not staged.is_file() or staged.is_symlink()
                or sha256(original.read_bytes()) != sha256(staged.read_bytes())):
            raise BuildError(f"staged tools differ from installed {TOOLS_VERSION}: {name}")


def write_cli_config(cache: Path, data_dir: Path) -> Path:
    for name in ("cache", "downloads", "user", "tmp"):
        (cache / name).mkdir(parents=True, exist_ok=True)
    config = {
        "directories": {"data": str(data_dir), "downloads": str(cache / "downloads"),
                        "user": str(cache / "user")},
        "build_cache": {"path": str(cache / "cache"), "extra_paths": []},
        "metrics": {"enabled": False},
        "updater": {"enable_notification": False},
        "sketch": {"always_export_binaries": False},
    }
    path = cache / "arduino-cli.json"
    path.write_text(json.dumps(config, indent=2) + "\n")
    return path


def compile_command(cli: str, config: Path, build: Path, tools: Path,
                    flags: str, jobs: int) -> list[str]:
    return [cli, "compile", "--config-file", str(config), "--fqbn", FQBN,
            "--build-path", str(build), "--jobs", str(jobs),
            "--build-property", f"runtime.tools.ameba_d_tools.path={tools}",
            "--build-property", f"compiler.c.extra_flags=-I{WORKSPACE / 'shared'}",
            "--build-property", f"compiler.c.elf.extra_flags={flags}",
            str(WORKSPACE / "nu87_ble_display_arduino")]


def clear_previous_images(build: Path, tools: Path) -> None:
    # Invalidate the previous snapshot before compiling. Never leave old images
    # looking like this attempt succeeded if compilation/postbuild later fails.
    for directory in (tools, build / "images"):
        for name in (*VENDOR_IMAGE_NAMES, MANIFEST_NAME):
            (directory / name).unlink(missing_ok=True)


def snapshot_images(build: Path, tools: Path, platform: Path, flags: str) -> Path:
    images = tuple((tools / name).read_bytes() for name in VENDOR_IMAGE_NAMES)
    # Reuse the flasher's strict SDK/slot/header/size validation before publishing
    # the snapshot. No payload checksum is invented by this structural check.
    validate_arduino_images(images)
    elf = build / "application.axf"
    if not elf.is_file() or not elf.stat().st_size:
        raise BuildError("successful compile did not produce application.axf")
    directory = build / "images"
    directory.mkdir(exist_ok=True)
    for name, data in zip(VENDOR_IMAGE_NAMES, images, strict=True):
        (directory / name).write_bytes(data)
    if load_arduino_images(build) != images:
        raise BuildError("image snapshot changed during validation")
    sources = sorted((WORKSPACE / "shared").glob("*.h"))
    sources += sorted(path for path in (WORKSPACE / "nu87_ble_display_arduino").iterdir()
                      if path.suffix in (".ino", ".h", ".cpp"))
    manifest = {
        "schema": 1, "image_format": "arduino", "sdk_version": SDK_VERSION,
        "tools_version": TOOLS_VERSION, "fqbn": FQBN,
        "compiler_c_elf_extra_flags": flags,
        "platform_sha256": sha256(platform.read_bytes()),
        "elf_sha256": sha256(elf.read_bytes()),
        "sources": {str(path.relative_to(WORKSPACE)): sha256(path.read_bytes())
                    for path in sources},
        "images": [{"name": name, "offset": offset, "bytes": len(data),
                    "sha256": sha256(data)}
                   for name, offset, data in zip(VENDOR_IMAGE_NAMES, (0, 0x4000, 0x6000),
                                                 images, strict=True)],
        "flash_invoked": False,
    }
    path = directory / MANIFEST_NAME
    path.write_text(json.dumps(manifest, indent=2) + "\n")
    return path


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arduino-data", type=Path, default=Path.home() / ".arduino15")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--build-dir", type=Path, default=WORKSPACE / "build/nu87-ble-native")
    parser.add_argument("--tools-dir", type=Path, default=WORKSPACE / "build/nu87-arduino-tools")
    parser.add_argument("--cache-dir", type=Path, default=WORKSPACE / "build/nu87-arduino-cache")
    parser.add_argument("--jobs", type=int, default=2)
    return parser


def execute(args: argparse.Namespace) -> int:
    if args.jobs < 1:
        raise BuildError("--jobs must be positive")
    build, tools, cache = map(workspace_output, (args.build_dir, args.tools_dir, args.cache_dir))
    for left, right in ((build, tools), (build, cache), (tools, cache)):
        if left.is_relative_to(right) or right.is_relative_to(left):
            raise BuildError("build, tools and cache directories must be separate")
    data_dir = args.arduino_data.expanduser().resolve()
    if not re.fullmatch(r"[A-Za-z0-9_./:+-]+", str(data_dir)):
        raise BuildError("vendor toolchain requires a data path without shell metacharacters")
    platform, flags = sdk_flags(data_dir)
    stage_tools(data_dir / f"packages/realtek/tools/ameba_d_tools/{TOOLS_VERSION}", tools)
    build.mkdir(parents=True, exist_ok=True)
    config = write_cli_config(cache, data_dir)
    clear_previous_images(build, tools)
    command = compile_command(args.arduino_cli, config, build, tools, flags, args.jobs)
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith("ARDUINO_")}
    environment.update(TMPDIR=str(cache / "tmp"), TMP=str(cache / "tmp"),
                       TEMP=str(cache / "tmp"), XDG_CACHE_HOME=str(cache / "cache"))
    result = subprocess.run(command, cwd=WORKSPACE, env=environment, check=False)
    if result.returncode != 0:
        raise BuildError(f"Arduino compile failed with exit {result.returncode}; no image snapshot")
    manifest = snapshot_images(build, tools, platform, flags)
    print(f"Validated image snapshot: {manifest.parent}")
    print(f"SHA-256 manifest: {manifest}")
    print("Build complete. No serial port was opened and no flash command was invoked.")
    return 0


def main(argv: list[str] | None = None) -> int:
    try:
        return execute(make_parser().parse_args(argv))
    except (BuildError, FlashError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("error: build interrupted; no completed image snapshot", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
