# SPDX-License-Identifier: Apache-2.0
"""Exercise isolated native build preparation and snapshots without compiling."""

import contextlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import nu87_native_build as native


class NativeBuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.workspace = self.root / "workspace"
        self.workspace.mkdir()
        self.patcher = patch.object(native, "WORKSPACE", self.workspace)
        self.patcher.start()
        self.addCleanup(self.patcher.stop)
        self.data = self.root / "installed"
        self.platform = (self.data / "packages/realtek/hardware/AmebaD/3.1.9/platform.txt")
        self.platform.parent.mkdir(parents=True)
        self.base_flags = "-Wl,-wrap,strcat -Wl,-wrap,malloc -Wl,--warn-common"
        self.platform.write_text(f"compiler.c.elf.extra_flags= {self.base_flags}\n")
        self.source_tools = self.data / "packages/realtek/tools/ameba_d_tools/1.1.3"
        for name in native.TOOL_PROGRAMS:
            program = self.source_tools / name
            program.parent.mkdir(parents=True, exist_ok=True)
            program.write_bytes(name.encode())
        self.build = self.workspace / "build/native"
        self.tools = self.workspace / "build/tools"
        self.cache = self.workspace / "build/cache"

    def arguments(self):
        return ["--arduino-data", str(self.data), "--build-dir", str(self.build),
                "--tools-dir", str(self.tools), "--cache-dir", str(self.cache)]

    def test_sdk_pin_preserves_all_linker_flags_and_appends_guards(self):
        platform, flags = native.sdk_flags(self.data)
        self.assertEqual(platform, self.platform)
        self.assertEqual(flags, self.base_flags + " " + native.OTP_FLAGS)
        other = self.platform.parent.parent / "3.2.0/platform.txt"
        other.parent.mkdir()
        other.write_text("other version")
        with self.assertRaisesRegex(native.BuildError, "only AmebaD 3.1.9"):
            native.sdk_flags(self.data)

    def test_missing_or_duplicate_linker_flags_fail(self):
        for content in ("version=3.1.X\n", "compiler.c.elf.extra_flags=\n",
                        "compiler.c.elf.extra_flags=a\ncompiler.c.elf.extra_flags=b\n"):
            with self.subTest(content=content):
                self.platform.write_text(content)
                with self.assertRaises(native.BuildError):
                    native.sdk_flags(self.data)

    def test_output_paths_cannot_escape_workspace_or_overlap(self):
        for path in (self.root / "outside", self.workspace / "build",
                     self.workspace / "build/with space"):
            with self.subTest(path=path), self.assertRaises(native.BuildError):
                native.workspace_output(path)
        with patch.object(native, "stage_tools") as stage, \
                contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(native.main(self.arguments() + ["--cache-dir", str(self.build)]), 1)
        stage.assert_not_called()

    def test_stage_tools_copies_source_without_modifying_it_and_rejects_changes(self):
        before = {str(path.relative_to(self.source_tools)): path.read_bytes()
                  for path in self.source_tools.rglob("*") if path.is_file()}
        native.stage_tools(self.source_tools, self.tools)
        self.assertEqual({str(path.relative_to(self.source_tools)): path.read_bytes()
                          for path in self.source_tools.rglob("*") if path.is_file()}, before)
        (self.tools / native.TOOL_PROGRAMS[0]).write_bytes(b"different")
        with self.assertRaises(native.BuildError):
            native.stage_tools(self.source_tools, self.tools)

    def test_compile_argv_config_and_environment_confine_outputs(self):
        manifest = self.build / "images/manifest.json"
        with patch.dict(os.environ, {"ARDUINO_DIRECTORIES_USER": "/outside",
                                    "ARDUINO_BUILD_CACHE_PATH": "/outside"}), \
                patch.object(native.subprocess, "run", return_value=SimpleNamespace(
                    returncode=0)) as run, \
                patch.object(native, "snapshot_images", return_value=manifest) as snapshot, \
                contextlib.redirect_stdout(io.StringIO()):
            result = native.main(self.arguments())
        self.assertEqual(result, 0)
        command = run.call_args.args[0]
        self.assertIsInstance(command, list)
        self.assertEqual(command[:2], ["arduino-cli", "compile"])
        self.assertIn(native.FQBN, command)
        self.assertIn(f"runtime.tools.ameba_d_tools.path={self.tools}", command)
        self.assertIn(f"compiler.c.extra_flags=-I{self.workspace / 'shared'}", command)
        self.assertIn("compiler.c.elf.extra_flags=" + self.base_flags + " " + native.OTP_FLAGS,
                      command)
        self.assertNotIn("--upload", command)
        self.assertNotIn("shell", run.call_args.kwargs)
        environment = run.call_args.kwargs["env"]
        self.assertFalse(any(key.startswith("ARDUINO_") for key in environment))
        for key in ("TMPDIR", "TMP", "TEMP"):
            self.assertEqual(environment[key], str(self.cache / "tmp"))
        config = json.loads((self.cache / "arduino-cli.json").read_text())
        self.assertEqual(config["directories"]["data"], str(self.data))
        for key in ("downloads", "user"):
            self.assertTrue(Path(config["directories"][key]).is_relative_to(self.cache))
        self.assertEqual(config["build_cache"]["path"], str(self.cache / "cache"))
        snapshot.assert_called_once()

    def test_failed_compile_invalidates_old_images_without_snapshot(self):
        for directory in (self.tools, self.build / "images"):
            directory.mkdir(parents=True)
            for name in (*native.VENDOR_IMAGE_NAMES, native.MANIFEST_NAME):
                (directory / name).write_bytes(b"old")
        # Complete the previously staged tools so only the compile result fails.
        for name in native.TOOL_PROGRAMS:
            path = self.tools / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes((self.source_tools / name).read_bytes())
        with patch.object(native.subprocess, "run", return_value=SimpleNamespace(
                returncode=1)), patch.object(native, "snapshot_images") as snapshot, \
                contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(native.main(self.arguments()), 1)
        snapshot.assert_not_called()
        self.assertFalse((self.build / "images/manifest.json").exists())
        self.assertFalse(any((self.tools / name).exists() for name in native.VENDOR_IMAGE_NAMES))

    def test_snapshot_copies_exact_bytes_and_records_hashes_after_validation(self):
        self.tools.mkdir(parents=True)
        self.build.mkdir(parents=True)
        (self.build / "application.axf").write_bytes(b"compiled-elf")
        (self.workspace / "shared").mkdir()
        (self.workspace / "shared/ble_display_protocol.h").write_bytes(b"protocol")
        (self.workspace / "shared/ble_face.h").write_bytes(b"face-protocol")
        sketch = self.workspace / "nu87_ble_display_arduino"
        sketch.mkdir()
        (sketch / "demo.ino").write_bytes(b"source")
        images = (b"km0", b"km4", b"image2")
        for name, data in zip(native.VENDOR_IMAGE_NAMES, images, strict=True):
            (self.tools / name).write_bytes(data)
        with patch.object(native, "validate_arduino_images") as validate, \
                patch.object(native, "load_arduino_images", return_value=images) as load:
            path = native.snapshot_images(self.build, self.tools, self.platform, "flags")
        validate.assert_called_once_with(images)
        load.assert_called_once_with(self.build)
        manifest = json.loads(path.read_text())
        self.assertFalse(manifest["flash_invoked"])
        self.assertEqual(manifest["sdk_version"], "3.1.9")
        self.assertEqual(manifest["elf_sha256"], native.sha256(b"compiled-elf"))
        self.assertEqual(manifest["sources"]["shared/ble_face.h"],
                         native.sha256(b"face-protocol"))
        self.assertEqual([entry["offset"] for entry in manifest["images"]], [0, 0x4000, 0x6000])
        for entry, data in zip(manifest["images"], images, strict=True):
            self.assertEqual(entry["sha256"], native.sha256(data))
            self.assertEqual((path.parent / entry["name"]).read_bytes(), data)

    def test_invalid_postbuild_output_cannot_publish_snapshot(self):
        self.tools.mkdir(parents=True)
        for name in native.VENDOR_IMAGE_NAMES:
            (self.tools / name).write_bytes(b"not-an-image")
        with self.assertRaises(native.FlashError):
            native.snapshot_images(self.build, self.tools, self.platform, "flags")
        self.assertFalse((self.build / "images").exists())


if __name__ == "__main__":
    unittest.main()
