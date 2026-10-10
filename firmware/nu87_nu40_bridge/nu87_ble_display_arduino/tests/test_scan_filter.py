# SPDX-License-Identifier: Apache-2.0
"""Compile the actual scan callback with a checking SDK parser substitute."""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

APP = Path(__file__).resolve().parents[1]


class NativeScanFilterTests(unittest.TestCase):
    def test_filter_bounds_sdk_sanitization_metadata_and_candidate_handover(self):
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("A host C++ compiler is required")
        source = (APP / "nu87_ble_display_arduino.ino").read_text()
        match = re.search(r"^static void on_scan\([^;]*?\)\n\{", source, re.MULTILINE)
        self.assertIsNotNone(match)
        depth = 0
        end = None
        for index in range(match.end() - 1, len(source)):
            depth += (source[index] == "{") - (source[index] == "}")
            if depth == 0:
                end = index + 1
                break
        self.assertIsNotNone(end)
        with tempfile.TemporaryDirectory(prefix="nu87-native-scan-") as directory:
            temporary = Path(directory)
            (temporary / "app_on_scan.h").write_text(source[match.start() : end])
            binary = temporary / "scan_filter"
            result = subprocess.run(
                [
                    compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie",
                    "-no-pie", "-I", str(temporary), str(APP / "tests/scan_filter.cpp"),
                    "-o", str(binary),
                ],
                capture_output=True, text=True, check=False, timeout=60,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            environment = os.environ.copy()
            environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
            environment["UBSAN_OPTIONS"] = "halt_on_error=1"
            result = subprocess.run(
                [str(binary)], capture_output=True, text=True, env=environment,
                check=False, timeout=5,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
