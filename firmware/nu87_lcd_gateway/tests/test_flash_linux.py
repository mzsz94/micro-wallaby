# SPDX-License-Identifier: Apache-2.0
"""Exercise flash boundaries, identity and staged vendor confirmation without hardware."""

import contextlib
import io
import struct
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

import importlib.util

spec = importlib.util.spec_from_file_location(
    "gateway_flash_linux", Path(__file__).resolve().parents[1] / "flash_linux.py")
flash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flash)


def bridge(device="/dev/ttyUSB0", serial="NU87-TEST", vid=flash.USB_VID, pid=flash.USB_PID):
    return SimpleNamespace(device=device, serial_number=serial, vid=vid, pid=pid)


def arduino_segment(address, payload=b"x"):
    return (flash.IMAGE2_MAGIC + struct.pack("<II", len(payload), address)
            + b"\xff" * 16 + payload)


def arduino_images(xip_size=1):
    km0 = (arduino_segment(0x0C000020, b"x" * (flash.ARDUINO_KM0_SIZE - 4096))
           + arduino_segment(0x00083000))
    km4 = (arduino_segment(0x0E000020, b"x" * xip_size)
           + arduino_segment(0x10005000) + arduino_segment(0x02000020, b""))
    padded = [part + b"\xff" * (-len(part) % 4096) for part in (km0, km4)]
    return b"pinned-km0-boot-fixture", b"pinned-km4-boot-fixture", b"".join(padded)


def arduino_boot_hashes(images):
    return patch.multiple(flash, ARDUINO_BOOT_SHA256=tuple(map(flash.sha256, images[:2])),
                          ARDUINO_KM0_SHA256=flash.sha256(images[2][:flash.ARDUINO_KM0_SIZE]))


class ImageTests(unittest.TestCase):
    def test_exact_boundaries_fit_and_overflow_is_rejected(self):
        flash.validate_images(b"b" * flash.APP_OFFSET,
                              b"a" * (flash.APP_LIMIT - flash.APP_OFFSET))
        for boot, app in ((b"", b"a"), (b"b", b""),
                          (b"b" * (flash.APP_OFFSET + 1), b"a"),
                          (b"b", b"a" * (flash.APP_LIMIT - flash.APP_OFFSET + 1))):
            with self.subTest(boot=len(boot), app=len(app)), self.assertRaises(flash.FlashError):
                flash.validate_images(boot, app)

    def test_repack_preserves_original_offsets_and_never_reaches_nvs(self):
        boot = bytes(range(256)) * 113
        app = b"application-image" * 100
        parts = flash.repack_images(boot, app)
        self.assertEqual(tuple(map(len, parts[:2])), (0x4000, 0x2000))
        flat = b"".join(parts)
        self.assertEqual(flat[:len(boot)], boot)
        self.assertEqual(flat[len(boot):flash.APP_OFFSET],
                         b"\xff" * (flash.APP_OFFSET - len(boot)))
        self.assertEqual(flat[flash.APP_OFFSET:], app)
        self.assertLess(len(flat), flash.STORAGE_OFFSET)

    def test_missing_and_empty_build_images_fail(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaises(flash.FlashError):
                flash.load_images(root)
            (root / "images").mkdir()
            (root / "images/bootloader_all.bin").write_bytes(b"b")
            (root / "images/km0_km4_app.bin").write_bytes(b"")
            with self.assertRaises(flash.FlashError):
                flash.load_images(root)


class IdentityTests(unittest.TestCase):
    def test_exact_bridge_identity_is_required(self):
        self.assertEqual(flash.validate_identity("/dev/ttyUSB0", "NU87-TEST", [bridge()]).vid,
                         flash.USB_VID)
        variants = ([bridge(serial="other")], [bridge(vid=0x239A)],
                    [bridge(pid=0x005A)], [bridge(device="/dev/ttyUSB1")], [], [bridge(), bridge()])
        for ports in variants:
            with self.subTest(ports=ports), self.assertRaises(flash.FlashError):
                flash.validate_identity("/dev/ttyUSB0", "NU87-TEST", ports)
        for port, serial in (("/dev/ttyACM0", "NU87-TEST"), ("/dev/ttyUSB0", "")):
            with self.assertRaises(flash.FlashError):
                flash.validate_identity(port, serial, [bridge()])

    def test_required_options_include_exact_serial(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            flash.make_parser().parse_args(["--build-dir", "build/test", "--port", "/dev/ttyUSB0"])

    def test_image_format_defaults_to_zephyr_and_rejects_unknown_layout(self):
        args = ["--build-dir", "build/test", "--port", "/dev/ttyUSB0",
                "--serial-number", "NU87-TEST"]
        self.assertEqual(flash.make_parser().parse_args(args).image_format, "zephyr")
        self.assertEqual(flash.make_parser().parse_args(
            args + ["--image-format", "arduino"]).image_format, "arduino")
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            flash.make_parser().parse_args(args + ["--image-format", "other"])


class ArduinoImageTests(unittest.TestCase):
    def test_slot_sizes_and_reserved_settings_boundary(self):
        size = flash.ARDUINO_APP_LIMIT - flash.IMAGE2_OFFSET
        # KM0 is pinned separately; KM4 has three headers and one RAM byte.
        images = arduino_images(xip_size=size - flash.ARDUINO_KM0_SIZE - 3 * 32 - 1)
        self.assertEqual(len(images[2]), size)
        with arduino_boot_hashes(images):
            flash.validate_arduino_images(images)
            for index, replacement in ((0, b"b" * (flash.KM4_BOOT_OFFSET + 1)),
                                       (1, b"b" * (flash.IMAGE2_OFFSET
                                                    - flash.KM4_BOOT_OFFSET + 1)),
                                       (2, images[2] + b"\xff" * 4096)):
                invalid = list(images)
                invalid[index] = replacement
                with self.subTest(index=index), self.assertRaises(flash.FlashError):
                    flash.validate_arduino_images(tuple(invalid))
        self.assertLess(flash.ARDUINO_APP_LIMIT, flash.ARDUINO_FTL_OFFSET)
        self.assertLess(flash.ARDUINO_FTL_END, flash.STORAGE_OFFSET)

    def test_unknown_boot_images_rejected_even_with_correct_image2(self):
        with self.assertRaisesRegex(flash.FlashError, "pinned"):
            flash.validate_arduino_images(arduino_images())

    def test_malformed_headers_lengths_padding_and_trailing_data_fail(self):
        images = arduino_images()
        app = images[2]
        corruptions = [b"wrong!!!" + app[8:], app[:20], app[:-1],
                       app[:8] + struct.pack("<I", len(app)) + app[12:],
                       app[:12] + struct.pack("<I", 0x0E000020) + app[16:],
                       app[:16] + b"\x00" + app[17:],
                       app[:8] + struct.pack("<I", 0) + app[12:],
                       app[:100] + b"\x00" + app[101:], app + b"\xff" * 4096]
        with arduino_boot_hashes(images):
            for damaged in corruptions:
                with self.subTest(size=len(damaged)), self.assertRaises(flash.FlashError):
                    flash.validate_arduino_images((*images[:2], damaged))

    def test_loader_requires_all_three_images_and_preserves_exact_bytes(self):
        images = arduino_images()
        with tempfile.TemporaryDirectory() as temporary, arduino_boot_hashes(images):
            build = Path(temporary)
            directory = build / "images"
            directory.mkdir()
            for name, data in zip(flash.VENDOR_IMAGE_NAMES, images, strict=True):
                (directory / name).write_bytes(data)
            self.assertEqual(flash.load_arduino_images(build), images)
            (directory / flash.VENDOR_IMAGE_NAMES[1]).unlink()
            with self.assertRaises(flash.FlashError):
                flash.load_arduino_images(build)


class StagingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.uploader = b"vendor-uploader"
        self.loader = b"vendor-amebad-loader"
        self.images = flash.repack_images(b"boot", b"app")

    def archive(self, *, missing_loader=False, symlink_loader=False):
        archive = self.root / "vendor.tar.gz"
        with tarfile.open(archive, "w:gz") as target:
            files = [(flash.UPLOADER_MEMBER, self.uploader)]
            if not missing_loader:
                files.append((flash.LOADER_MEMBER, self.loader))
            files.append(("ameba_d_tools_linux/Settings.json", b'{"erase":true,"otp":true}'))
            for name, data in files:
                member = tarfile.TarInfo(name)
                member.size = len(data)
                if name == flash.LOADER_MEMBER and symlink_loader:
                    member.type = tarfile.SYMTYPE
                    member.linkname = "/tmp/untrusted-loader"
                target.addfile(member, io.BytesIO(data))
        return archive

    @contextlib.contextmanager
    def expected_hashes(self, archive):
        with patch.multiple(flash, ARCHIVE_SHA256=flash.sha256(archive.read_bytes()),
                            UPLOADER_SHA256=flash.sha256(self.uploader),
                            LOADER_SHA256=flash.sha256(self.loader)):
            yield

    def test_staging_copies_only_pinned_tool_loader_and_exact_images(self):
        archive = self.archive()
        original = archive.read_bytes()
        with self.expected_hashes(archive):
            uploader = flash.stage_bundle(archive, self.root / "bundle", self.images)
        self.assertEqual(archive.read_bytes(), original)
        self.assertEqual(uploader.read_bytes(), self.uploader)
        self.assertTrue(uploader.stat().st_mode & 0o100)
        self.assertEqual({p.name for p in uploader.parent.iterdir()},
                         {flash.UPLOADER_NAME, flash.LOADER_NAME, *flash.VENDOR_IMAGE_NAMES})
        flash.verify_staged_images(uploader.parent, self.images)
        (uploader.parent / flash.VENDOR_IMAGE_NAMES[2]).write_bytes(b"changed")
        with self.assertRaises(flash.FlashError):
            flash.verify_staged_images(uploader.parent, self.images)

    def test_missing_or_symlink_loader_fails_before_creating_bundle(self):
        for options in ({"missing_loader": True}, {"symlink_loader": True}):
            with self.subTest(options=options):
                archive = self.archive(**options)
                destination = self.root / "bundle"
                with self.expected_hashes(archive), self.assertRaises(flash.FlashError):
                    flash.stage_bundle(archive, destination, self.images)
                self.assertFalse(destination.exists())

    def test_archive_or_loader_hash_mismatch_fails(self):
        archive = self.archive()
        with self.assertRaises(flash.FlashError):
            flash.stage_bundle(archive, self.root / "bundle", self.images)
        with self.expected_hashes(archive), patch.object(flash, "LOADER_SHA256", "0" * 64), \
                self.assertRaises(flash.FlashError):
            flash.stage_bundle(archive, self.root / "bundle", self.images)

    def test_default_dry_run_stages_but_never_opens_or_launches(self):
        archive = self.archive()
        build = self.root / "build"
        (build / "images").mkdir(parents=True)
        (build / "images/bootloader_all.bin").write_bytes(b"boot")
        (build / "images/km0_km4_app.bin").write_bytes(b"app")
        with self.expected_hashes(archive), \
                patch.object(flash, "validate_identity", return_value=bridge()), \
                patch.object(flash, "inactive_modem_lines") as modem, \
                patch.object(flash, "run_uploader") as uploader, \
                patch("builtins.input") as prompt, \
                contextlib.redirect_stdout(io.StringIO()) as output:
            result = flash.main(["--build-dir", str(build), "--port", "/dev/ttyUSB0",
                                 "--serial-number", "NU87-TEST", "--archive", str(archive)])
        self.assertEqual(result, 0)
        self.assertIn("DRY RUN", output.getvalue())
        modem.assert_not_called()
        uploader.assert_not_called()
        prompt.assert_not_called()

    def test_auto_boot_is_opt_in_and_dry_run_still_never_opens(self):
        archive = self.archive()
        build = self.root / "build"
        (build / "images").mkdir(parents=True)
        (build / "images/bootloader_all.bin").write_bytes(b"boot")
        (build / "images/km0_km4_app.bin").write_bytes(b"app")
        args = ["--build-dir", str(build), "--port", "/dev/ttyUSB0",
                "--serial-number", "NU87-TEST", "--archive", str(archive)]
        self.assertFalse(flash.make_parser().parse_args(args).auto_boot)
        with self.expected_hashes(archive), \
                patch.object(flash, "validate_identity", return_value=bridge()), \
                patch.object(flash, "inactive_modem_lines") as modem, \
                patch.object(flash, "run_uploader") as uploader, \
                patch("builtins.input") as prompt, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(flash.main(args + ["--auto-boot"]), 0)
        modem.assert_not_called()
        uploader.assert_not_called()
        prompt.assert_not_called()

    def test_auto_write_revalidates_identity_without_manual_guard_or_prompt(self):
        archive = self.archive()
        build = self.root / "build"
        (build / "images").mkdir(parents=True)
        (build / "images/bootloader_all.bin").write_bytes(b"boot")
        (build / "images/km0_km4_app.bin").write_bytes(b"app")

        def upload(command, bundle):
            self.assertEqual(command[-3:], ["Enable", "Disable", str(flash.DEFAULT_BAUD)])
            flash.verify_staged_images(bundle, self.images)
            return 0, flash.SUCCESS_MARKER

        with self.expected_hashes(archive), \
                patch.object(flash, "validate_identity", return_value=bridge()) as identity, \
                patch.object(flash, "inactive_modem_lines") as modem, \
                patch("builtins.input") as prompt, \
                patch.object(flash, "run_uploader", side_effect=upload) as uploader, \
                contextlib.redirect_stdout(io.StringIO()) as output:
            result = flash.main(["--build-dir", str(build), "--port", "/dev/ttyUSB0",
                                 "--serial-number", "NU87-TEST", "--archive", str(archive),
                                 "--write", "--auto-boot"])
        self.assertEqual(result, 0)
        self.assertEqual(identity.call_count, 2)
        self.assertTrue(all(call.kwargs["require_device"] for call in identity.call_args_list))
        modem.assert_not_called()
        prompt.assert_not_called()
        uploader.assert_called_once()
        self.assertIn("flash readback and firmware operation unverified", output.getvalue())

    def test_arduino_dry_run_uses_exact_native_slots_without_repack_or_port_open(self):
        archive = self.archive()
        images = arduino_images()
        build = self.root / "arduino-build"
        (build / "images").mkdir(parents=True)
        for name, data in zip(flash.VENDOR_IMAGE_NAMES, images, strict=True):
            (build / "images" / name).write_bytes(data)
        original_stage = flash.stage_bundle
        recorded = []

        def stage(source, destination, actual):
            result = original_stage(source, destination, actual)
            recorded.append(tuple((destination / name).read_bytes()
                                  for name in flash.VENDOR_IMAGE_NAMES))
            return result

        with self.expected_hashes(archive), arduino_boot_hashes(images), \
                patch.object(flash, "validate_identity", return_value=bridge()), \
                patch.object(flash, "stage_bundle", side_effect=stage), \
                patch.object(flash, "repack_images") as repack, \
                patch.object(flash, "inactive_modem_lines") as modem, \
                patch.object(flash, "run_uploader") as uploader, \
                patch("builtins.input") as prompt, \
                contextlib.redirect_stdout(io.StringIO()) as output:
            result = flash.main(["--build-dir", str(build), "--image-format", "arduino",
                                 "--port", "/dev/ttyUSB0", "--serial-number", "NU87-TEST",
                                 "--archive", str(archive)])
        self.assertEqual(result, 0)
        self.assertEqual(recorded, [images])
        for offset in ("0x000000", "0x004000", "0x006000"):
            self.assertIn(f"offset={offset}", output.getvalue())
        self.assertIn("0x102000..0x104fff", output.getvalue())
        self.assertIn("DRY RUN", output.getvalue())
        repack.assert_not_called()
        modem.assert_not_called()
        uploader.assert_not_called()
        prompt.assert_not_called()

    def test_arduino_mock_write_revalidates_identity_and_staged_images(self):
        archive = self.archive()
        images = arduino_images()
        build = self.root / "arduino-build"
        (build / "images").mkdir(parents=True)
        for name, data in zip(flash.VENDOR_IMAGE_NAMES, images, strict=True):
            (build / "images" / name).write_bytes(data)

        def upload(command, bundle):
            self.assertEqual(command[-3:], ["Disable", "Disable", str(flash.DEFAULT_BAUD)])
            self.assertEqual(tuple((bundle / name).read_bytes()
                                   for name in flash.VENDOR_IMAGE_NAMES), images)
            return 0, flash.SUCCESS_MARKER

        with self.expected_hashes(archive), arduino_boot_hashes(images), \
                patch.object(flash, "validate_identity", return_value=bridge()) as identity, \
                patch.object(flash, "inactive_modem_lines",
                             return_value=contextlib.nullcontext()) as guard, \
                patch("builtins.input", return_value="") as prompt, \
                patch.object(flash, "run_uploader", side_effect=upload) as uploader, \
                patch.object(flash, "repack_images") as repack, \
                contextlib.redirect_stdout(io.StringIO()) as output:
            result = flash.main(["--build-dir", str(build), "--image-format", "arduino",
                                 "--port", "/dev/ttyUSB0", "--serial-number", "NU87-TEST",
                                 "--archive", str(archive), "--write"])
        self.assertEqual(result, 0)
        self.assertEqual(identity.call_count, 2)
        for call in identity.call_args_list:
            self.assertTrue(call.kwargs["require_device"])
        guard.assert_called_once_with("/dev/ttyUSB0")
        prompt.assert_called_once()
        uploader.assert_called_once()
        repack.assert_not_called()
        self.assertIn("flash readback and firmware operation unverified", output.getvalue())

    def test_write_exit_zero_without_marker_returns_failure(self):
        archive = self.archive()
        build = self.root / "build"
        (build / "images").mkdir(parents=True)
        (build / "images/bootloader_all.bin").write_bytes(b"boot")
        (build / "images/km0_km4_app.bin").write_bytes(b"app")
        with self.expected_hashes(archive), \
                patch.object(flash, "validate_identity", return_value=bridge()) as identity, \
                patch.object(flash, "inactive_modem_lines",
                             return_value=contextlib.nullcontext()), \
                patch("builtins.input", return_value="") as prompt, \
                patch.object(flash, "run_uploader", return_value=(0, "Download fail")), \
                contextlib.redirect_stdout(io.StringIO()) as output, \
                contextlib.redirect_stderr(io.StringIO()) as errors:
            result = flash.main(["--build-dir", str(build), "--port", "/dev/ttyUSB0",
                                 "--serial-number", "NU87-TEST", "--archive", str(archive),
                                 "--write"])
        self.assertEqual(result, 1)
        self.assertIn("upload unconfirmed", errors.getvalue())
        self.assertNotIn("Vendor confirmed upload", output.getvalue())
        self.assertEqual(identity.call_count, 2)
        self.assertTrue(identity.call_args.kwargs["require_device"])
        prompt.assert_called_once()

    def test_manual_boot_occurs_inside_guard_before_revalidation_and_vendor(self):
        archive = self.archive()
        build = self.root / "build"
        (build / "images").mkdir(parents=True)
        (build / "images/bootloader_all.bin").write_bytes(b"boot")
        (build / "images/km0_km4_app.bin").write_bytes(b"app")
        events = []
        original_verify = flash.verify_staged_images

        @contextlib.contextmanager
        def guard(port):
            self.assertEqual(port, "/dev/ttyUSB0")
            events.append("guard-open")
            try:
                yield
            finally:
                events.append("guard-close")

        def identify(*_args, **kwargs):
            self.assertTrue(kwargs["require_device"])
            events.append("identity")
            return bridge()

        def prompt(_message):
            events.append("prompt")
            return ""

        def verify(*args):
            events.append("verify")
            return original_verify(*args)

        def upload(*_args, **_kwargs):
            events.append("vendor")
            return 0, flash.SUCCESS_MARKER

        with self.expected_hashes(archive), \
                patch.object(flash, "validate_identity", side_effect=identify) as identity, \
                patch.object(flash, "inactive_modem_lines", side_effect=guard), \
                patch("builtins.input", side_effect=prompt) as input_fn, \
                patch.object(flash, "verify_staged_images", side_effect=verify), \
                patch.object(flash, "run_uploader", side_effect=upload) as uploader, \
                contextlib.redirect_stdout(io.StringIO()):
            result = flash.main(["--build-dir", str(build), "--port", "/dev/ttyUSB0",
                                 "--serial-number", "NU87-TEST", "--archive", str(archive),
                                 "--write"])
        self.assertEqual(result, 0)
        self.assertEqual(events[events.index("guard-open"):],
                         ["guard-open", "prompt", "identity", "verify", "vendor", "guard-close"])
        self.assertEqual(identity.call_count, 2)
        input_fn.assert_called_once()
        uploader.assert_called_once()

    def test_manual_boot_eof_or_ctrl_c_cannot_implicitly_start_uploader(self):
        archive = self.archive()
        build = self.root / "build"
        (build / "images").mkdir(parents=True)
        (build / "images/bootloader_all.bin").write_bytes(b"boot")
        (build / "images/km0_km4_app.bin").write_bytes(b"app")
        for error in (EOFError(), KeyboardInterrupt()):
            events = []

            @contextlib.contextmanager
            def guard(_port, recorded=events):
                recorded.append("guard-open")
                try:
                    yield
                finally:
                    recorded.append("guard-close")

            with self.subTest(error=type(error).__name__), self.expected_hashes(archive), \
                    patch.object(flash, "validate_identity", return_value=bridge()) as identity, \
                    patch.object(flash, "inactive_modem_lines", side_effect=guard), \
                    patch("builtins.input", side_effect=error) as prompt, \
                    patch.object(flash, "run_uploader") as uploader, \
                    contextlib.redirect_stdout(io.StringIO()) as output, \
                    contextlib.redirect_stderr(io.StringIO()) as errors:
                result = flash.main(["--build-dir", str(build), "--port", "/dev/ttyUSB0",
                                     "--serial-number", "NU87-TEST", "--archive", str(archive),
                                     "--write"])
            self.assertEqual(result, 1)
            self.assertEqual(events, ["guard-open", "guard-close"])
            self.assertEqual(identity.call_count, 1)
            prompt.assert_called_once()
            uploader.assert_not_called()
            self.assertIn("error:", errors.getvalue())
            self.assertNotIn("Vendor confirmed upload", output.getvalue())


class UploadTests(unittest.TestCase):
    def test_manual_boot_wait_uses_injected_or_default_input(self):
        injected = MagicMock(return_value="")
        with patch("builtins.input") as default, contextlib.redirect_stdout(io.StringIO()):
            flash.wait_for_manual_boot(input_fn=injected)
        injected.assert_called_once()
        default.assert_not_called()
        with patch("builtins.input", return_value="") as default, \
                contextlib.redirect_stdout(io.StringIO()):
            flash.wait_for_manual_boot()
        default.assert_called_once()

    def test_manual_boot_wait_converts_eof_and_interrupt_to_flash_error(self):
        for error in (EOFError(), KeyboardInterrupt()):
            with self.subTest(error=type(error).__name__), \
                    patch("builtins.input", side_effect=error), \
                    contextlib.redirect_stdout(io.StringIO()), self.assertRaises(flash.FlashError):
                flash.wait_for_manual_boot()

    def test_command_disables_auto_upload_and_whole_flash_erase(self):
        command = flash.uploader_command(Path("/tmp/bundle"), "/dev/ttyUSB0", 115200)
        self.assertEqual(command[2:],
                         ["/dev/ttyUSB0", "Ameba_AMB26", "Disable", "Disable", "115200"])
        with self.assertRaises(flash.FlashError):
            flash.uploader_command(Path("/tmp/bundle"), "/dev/ttyUSB0", 9600)

    def test_auto_command_keeps_whole_chip_erase_disabled(self):
        command = flash.uploader_command(Path("/tmp/bundle"), "/dev/ttyUSB0", 115200,
                                         auto_boot=True)
        self.assertEqual(command[-3:], ["Enable", "Disable", "115200"])

    def test_exit_zero_without_success_and_errors_with_marker_fail(self):
        for code, output in ((0, ""), (0, "Images download fail!"),
                             (1, flash.SUCCESS_MARKER),
                             (0, "Err:checksum error\n" + flash.SUCCESS_MARKER)):
            with self.subTest(code=code, output=output):
                self.assertFalse(flash.upload_succeeded(code, output))
        self.assertTrue(flash.upload_succeeded(0, flash.SUCCESS_MARKER))

    def test_inactive_modem_lines_are_set_before_open_and_handle_is_closed(self):
        events = []

        class DummySerial:
            def __init__(self, **kwargs):
                self.assertions = kwargs

            def __setattr__(self, name, value):
                events.append((name, value))
                object.__setattr__(self, name, value)

            def open(self):
                events.append(("open", None))

            def close(self):
                events.append(("close", None))

        with patch("serial.Serial", DummySerial), flash.inactive_modem_lines("/dev/ttyUSB0"):
            self.assertIn(("open", None), events)
            self.assertNotIn(("close", None), events)
        self.assertLess(events.index(("dtr", False)), events.index(("open", None)))
        self.assertLess(events.index(("rts", False)), events.index(("open", None)))
        self.assertEqual(events[-1], ("close", None))

    def test_timeout_kills_vendor_process_and_cannot_be_success(self):
        process = MagicMock()
        process.communicate.side_effect = [subprocess.TimeoutExpired("vendor", 1),
                                           ("partial", None)]
        with patch.object(flash.subprocess, "Popen", return_value=process), \
                contextlib.redirect_stdout(io.StringIO()), self.assertRaises(flash.FlashError):
            flash.run_uploader(["unused-vendor"], Path("/tmp"), timeout=1)
        process.kill.assert_called_once()


if __name__ == "__main__":
    unittest.main()
