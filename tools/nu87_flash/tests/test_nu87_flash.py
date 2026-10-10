from __future__ import annotations

import argparse
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


MODULE_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(MODULE_DIR))
import nu87_flash as flash  # noqa: E402


class RepackTests(unittest.TestCase):
    def test_repack_preserves_ranges_gap_and_tail(self) -> None:
        boot = bytes(index % 251 for index in range(0x7003))
        app = b"APP-CONTENT-END"

        first, second, third = flash.repack_images(boot, app)
        joined = first + second + third

        self.assertEqual(len(first), 0x4000)
        self.assertEqual(len(second), 0x2000)
        self.assertEqual(joined[: len(boot)], boot)
        self.assertEqual(
            joined[len(boot) : flash.APP_OFFSET],
            b"\xff" * (flash.APP_OFFSET - len(boot)),
        )
        self.assertEqual(joined[flash.APP_OFFSET :], app)
        self.assertEqual(third[-len(app) :], app)

    def test_short_boot_is_padded_across_fixed_slices(self) -> None:
        first, second, third = flash.repack_images(b"BOOT", b"A")
        self.assertEqual(first[:4], b"BOOT")
        self.assertEqual(first[4:], b"\xff" * (0x4000 - 4))
        self.assertEqual(second, b"\xff" * 0x2000)
        self.assertEqual(third[: flash.APP_OFFSET - 0x6000], b"\xff" * 0xE000)

    def test_empty_images_are_rejected(self) -> None:
        for boot, app in ((b"", b"app"), (b"boot", b"")):
            with self.subTest(boot=bool(boot), app=bool(app)):
                with self.assertRaises(flash.FlashError):
                    flash.repack_images(boot, app)

    def test_oversize_images_are_rejected(self) -> None:
        with self.assertRaises(flash.FlashError):
            flash.repack_images(b"b" * (flash.APP_OFFSET + 1), b"a")
        with self.assertRaises(flash.FlashError):
            flash.repack_images(
                b"b",
                b"a" * (flash.FLASH_SIZE - flash.APP_OFFSET + 1),
            )


class CommandTests(unittest.TestCase):
    def test_apple_silicon_command_uses_rosetta(self) -> None:
        bundle = Path("/tmp/test-bundle")
        command = flash.build_uploader_command(
            bundle, "/dev/cu.usbserial-test", 115200, machine="arm64"
        )
        self.assertEqual(command[:2], ["arch", "-x86_64"])
        self.assertEqual(
            command[2:],
            [
                str(bundle / "upload_image_tool_macos"),
                str(bundle),
                "/dev/cu.usbserial-test",
                "Ameba_AMB26",
                "Disable",
                "Disable",
                "115200",
            ],
        )

    def test_intel_command_runs_uploader_directly(self) -> None:
        bundle = Path("/tmp/test-bundle")
        command = flash.build_uploader_command(
            bundle, "/dev/cu.test", 921600, machine="x86_64"
        )
        self.assertEqual(command[0], str(bundle / "upload_image_tool_macos"))

    def test_success_requires_exit_zero_and_marker(self) -> None:
        self.assertTrue(flash.upload_succeeded(0, flash.SUCCESS_MARKER))
        self.assertFalse(flash.upload_succeeded(1, flash.SUCCESS_MARKER))
        self.assertFalse(flash.upload_succeeded(0, "done"))

    def test_unsupported_baud_is_rejected(self) -> None:
        with self.assertRaises(flash.FlashError):
            flash.build_uploader_command(Path("/tmp/b"), "/dev/cu.x", 460800)


class DryRunTests(unittest.TestCase):
    def test_dry_run_never_downloads_or_starts_uploader(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            images = build / "images"
            images.mkdir()
            (images / "bootloader_all.bin").write_bytes(b"boot")
            (images / "km0_km4_app.bin").write_bytes(b"app")
            args = argparse.Namespace(
                build_dir=build,
                port="/dev/cu.test",
                archive=None,
                baud=115200,
                write=False,
            )
            with (
                mock.patch.object(flash.platform, "system", return_value="Darwin"),
                mock.patch.object(flash, "download_archive") as download,
                mock.patch.object(flash, "run_uploader") as run,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(flash.execute(args), 0)
            download.assert_not_called()
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
