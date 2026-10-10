"""Streaming state regressions; no physical device or server is needed."""

from pathlib import Path
from types import SimpleNamespace
import json
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from imu_bridge import Bridge


class BridgeStreamTests(unittest.TestCase):
    def write_reference(self, record):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "reference.json"
        path.write_text(json.dumps(record), encoding="utf-8")
        return path

    def test_orientation_reference_is_explicit_bound_to_device_and_detached(self):
        record = {"board": "nu87", "serialNumber": "target", "gravity": [0.1, 0.98, -0.05],
                  "label": "USB-C 위 기준", "capturedAt": "2026-10-10T07:00:00Z",
                  "calibration": {"samples": 100}}
        path = self.write_reference(record)
        bridge = Bridge("target", board="nu87", orientation_reference=path)
        bridge.feed("sample=1 accel_g=0.2,0.9,-0.1 gyro_dps=1,2,3 corrected_dps=0.1,0.2,0.3")
        snapshot = bridge.snapshot()
        self.assertEqual(snapshot["orientationReference"], {"gravity": record["gravity"], "label": record["label"]})
        self.assertEqual(snapshot["samples"][0]["accel"], [0.2, 0.9, -0.1])
        self.assertEqual(snapshot["samples"][0]["gyro"], [1, 2, 3])
        self.assertEqual(snapshot["samples"][0]["corrected"], [0.1, 0.2, 0.3])
        self.assertFalse(snapshot["sensor"]["biasValid"])
        snapshot["orientationReference"]["gravity"][0] = 99
        self.assertEqual(bridge.snapshot()["orientationReference"]["gravity"][0], .1)
        self.assertIsNone(Bridge("target", board="nu87").snapshot()["orientationReference"])
        for board, serial_number in [("nu40", "target"), ("nu87", "other")]:
            with self.subTest(board=board, serial_number=serial_number), self.assertRaises(ValueError):
                Bridge(serial_number, board=board, orientation_reference=path)

    def test_orientation_reference_rejects_invalid_gravity_and_identity(self):
        valid = {"board": "nu87", "serialNumber": "target", "gravity": [0, 1, 0], "label": "USB-C 위 기준"}
        invalid = [{**valid, "gravity": value} for value in (
            [0, 0, 0], [0, 0, 9.81], [float("nan"), 0, 1], [True, 0, 1], ["0", 0, 1], [0, 1])]
        invalid += [{**valid, "board": "nu40"}, {**valid, "serialNumber": None},
                    {**valid, "label": ""}, {**valid, "label": 123}, []]
        for record in invalid:
            with self.subTest(record=record), self.assertRaises(ValueError):
                Bridge("target", board="nu87", orientation_reference=self.write_reference(record))

    def test_host_bias_uses_raw_gyro_once_without_changing_firmware_state(self):
        record = {"board": "nu87", "serialNumber": "target", "gravity": [0, 1, 0], "label": "USB-C 위 기준",
                  "calibrationSource": "host", "gyroBiasDps": [.25, -.5, 1]}
        bridge = Bridge("target", board="nu87", orientation_reference=self.write_reference(record))
        bridge.feed("sample=1 accel_g=0.2,0.9,-0.1 gyro_dps=1,2,3 corrected_dps=9,8,7")
        protocol_before = bridge.protocol.snapshot()
        first = bridge.snapshot()
        second = bridge.snapshot()
        for snapshot in (first, second):
            self.assertEqual(snapshot["samples"][0]["accel"], [.2, .9, -.1])
            self.assertEqual(snapshot["samples"][0]["gyro"], [1, 2, 3])
            self.assertEqual(snapshot["samples"][0]["corrected"], [.75, 2.5, 2])
            self.assertEqual(snapshot["calibration"], {"source": "host", "label": "맥북 보정 적용", "gyroBiasDps": [.25, -.5, 1]})
            self.assertFalse(snapshot["sensor"]["biasValid"])
        self.assertEqual(bridge.protocol.snapshot(), protocol_before)
        first["calibration"]["gyroBiasDps"][0] = 99
        self.assertEqual(bridge.snapshot()["calibration"]["gyroBiasDps"][0], .25)

    def test_host_bias_requires_explicit_source_and_finite_small_offsets(self):
        valid = {"board": "nu87", "serialNumber": "target", "gravity": [0, 1, 0], "label": "USB-C 위 기준",
                 "calibrationSource": "host", "gyroBiasDps": [0, 0, 0]}
        invalid = [{**valid, "gyroBiasDps": bias} for bias in (
            None, [0, 1], [True, 0, 0], ["0", 0, 0], [float("inf"), 0, 0], [0, -5.01, 0], [0, 0, 5.01])]
        invalid.append({**valid, "calibrationSource": "firmware"})
        for record in invalid:
            with self.subTest(record=record), self.assertRaises(ValueError):
                Bridge("target", board="nu87", orientation_reference=self.write_reference(record))

    def test_flat_frame_adds_physical_mount_reference_without_changing_gyro_bias(self):
        record = {"board": "nu87", "serialNumber": "target", "gravity": [-.7229258, -.0906288, .7499919],
                  "flatGravity": [.10801, .05232, 1.041732], "flatCapturedAt": "2026-10-10T12:00:00Z",
                  "label": "USB-C 위 기준", "calibrationSource": "host", "gyroBiasDps": [.2, -.3, .1]}
        bridge = Bridge("target", board="nu87", orientation_reference=self.write_reference(record))
        state = bridge.snapshot()
        self.assertEqual(state["orientationReference"], {"gravity": record["gravity"], "flatGravity": record["flatGravity"],
                                                        "label": "바닥 기준 · USB-C 축 정렬"})
        self.assertEqual(state["calibration"]["gyroBiasDps"], record["gyroBiasDps"])
        state["orientationReference"]["flatGravity"][0] = 99
        self.assertEqual(bridge.snapshot()["orientationReference"]["flatGravity"][0], .10801)
        with self.assertRaises(ValueError):
            Bridge("other", board="nu87", orientation_reference=self.write_reference(record))

    def test_flat_frame_rejects_invalid_or_parallel_captures(self):
        base = {"board": "nu87", "serialNumber": "target", "gravity": [0, .8, .6], "label": "USB-C 위 기준"}
        for flat in ([0, 0, 0], [0, 0, 9.81], [False, 0, 1], [float("nan"), 0, 1], [0, 1], [0, .8, .6], [0, -.8, -.6]):
            with self.subTest(flat=flat), self.assertRaises(ValueError):
                Bridge("target", board="nu87", orientation_reference=self.write_reference({**base, "flatGravity": flat}))

    def test_default_nu40_metadata_and_detached_snapshot(self):
        bridge = Bridge()
        first = bridge.snapshot()
        self.assertEqual(first["device"], {
            "board": "nu40", "name": "NU40-DK", "mcu": "nRF52840",
            "pins": {"sda": "P0.26", "scl": "P0.27"}, "baud": 115200,
        })
        first["device"]["pins"]["sda"] = "modified"
        self.assertEqual(bridge.snapshot()["device"]["pins"]["sda"], "P0.26")

    def test_nu87_requires_an_explicit_usb_identity(self):
        for serial_number in (None, "", "  "):
            with self.subTest(serial_number=serial_number), self.assertRaises(ValueError):
                Bridge(serial_number, board="nu87")
        with self.assertRaises(ValueError):
            Bridge(board="unsupported")

    def test_nu87_filters_both_usb_chip_and_exact_serial(self):
        bridge = Bridge("target", board="nu87")
        ports = [
            SimpleNamespace(vid=0x10C4, pid=0xEA60, serial_number="other", device="/dev/other-cp2102"),
            SimpleNamespace(vid=0x2FE3, pid=0x0004, serial_number="target", device="/dev/nu40"),
            SimpleNamespace(vid=0x10C4, pid=0xEA60, serial_number="target", device="/dev/nu87"),
        ]
        with patch("imu_bridge.list_ports.comports", return_value=ports):
            self.assertEqual(bridge.find_port(), "/dev/nu87")
        with patch("imu_bridge.list_ports.comports", return_value=ports[:2]):
            self.assertIsNone(bridge.find_port())
        with patch("imu_bridge.list_ports.comports", return_value=[ports[2], ports[2]]):
            with self.assertRaises(RuntimeError):
                bridge.find_port()
        self.assertEqual(bridge.snapshot()["device"], {
            "board": "nu87", "name": "NU-87", "mcu": "RTL8720DF",
            "pins": {"sda": "PA26", "scl": "PA25"}, "baud": 1500000,
        })

    def test_nu40_default_does_not_open_cp2102_devices(self):
        ports = [SimpleNamespace(vid=0x10C4, pid=0xEA60, serial_number="target", device="/dev/nu87")]
        with patch("imu_bridge.list_ports.comports", return_value=ports):
            self.assertIsNone(Bridge().find_port())

    def test_nu87_opens_selected_port_at_1500000_baud(self):
        bridge = Bridge("target", board="nu87")
        with patch.object(bridge, "find_port", return_value="/dev/fake-nu87"), patch("imu_bridge.serial.Serial") as serial_port:
            device = serial_port.return_value.__enter__.return_value
            device.in_waiting = 0
            def finish_read(_count):
                bridge.stop.set()
                return b""
            device.read.side_effect = finish_read
            bridge.run()
            serial_port.assert_called_once_with("/dev/fake-nu87", 1500000, timeout=0.1, write_timeout=1, exclusive=True)
            device.write.assert_not_called()

    def test_nu87_commands_are_explicitly_unavailable(self):
        bridge = Bridge("target", board="nu87")
        bridge.set_connection("connected", "/dev/fake-nu87", "connected")
        with self.assertRaisesRegex(ConnectionError, "보드 명령"):
            bridge.enqueue("imu status")
        self.assertTrue(bridge.commands.empty())

    def test_unchanged_state_does_not_resend_history(self):
        bridge = Bridge()
        first = bridge.snapshot()
        with patch.object(bridge.protocol, "snapshot") as copy_history:
            self.assertIsNone(bridge.snapshot(after_revision=first["revision"]))
            copy_history.assert_not_called()
        bridge.feed("unrelated console output")
        self.assertIsNone(bridge.snapshot(after_revision=first["revision"]))

    def test_updates_in_same_millisecond_are_not_lost(self):
        bridge = Bridge()
        with patch("imu_bridge.time.time", return_value=123):
            bridge.set_connection("connected", "/dev/example", "connected")
            first = bridge.snapshot()
            bridge.feed("sample=1 accel_g=0,0,1 gyro_dps=1,2,3 corrected_dps=1,2,3")
            second = bridge.snapshot(after_revision=first["revision"])
        self.assertEqual(first["updatedAt"], second["updatedAt"])
        self.assertGreater(second["revision"], first["revision"])
        self.assertEqual(second["samples"][-1]["seq"], 1)
        bridge.set_connection("disconnected", None, "disconnected")
        third = bridge.snapshot(after_revision=second["revision"])
        self.assertFalse(third["sensor"]["ready"])
        self.assertEqual(third["samples"], second["samples"])


if __name__ == "__main__":
    unittest.main()
