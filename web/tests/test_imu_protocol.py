"""Protocol tests use the firmware's actual console records and message formats."""

import importlib.util
from pathlib import Path
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("imu_protocol", ROOT / "web/tools/imu_protocol.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
ImuProtocol = MODULE.ImuProtocol


def sample(seq=1):
    return f"sample={seq} accel_g=-0.1250,+0.2500,0.9980 gyro_dps=-12.5000,3.7500,-0.0500 corrected_dps=-12.0000,3.5000,0.0000"


class ImuProtocolTests(unittest.TestCase):
    def setUp(self):
        self.parser = ImuProtocol()

    def test_actual_timeout_capture_never_creates_a_measurement(self):
        path = ROOT / "build/nu40-imu/hardware-v1.1.log"
        # The build folder is optional in a fresh checkout; preserve representative
        # lines here so this regression still covers the observed hardware failure.
        fallback = "\n".join([
            "NU40-DK IMU v1.1 (Sep 19 2026 16:39:41)",
            "I2C idle before probe: SDA=1 SCL=0 (expected 1/1)",
            "probe addr=0x6a WHO_AM_I=unavailable read_error=-116 stage=read-WHO_AM_I",
            "probe addr=0x6b WHO_AM_I=unavailable read_error=-116 stage=read-WHO_AM_I",
            "ready=0 address=0x6b profile=unidentified last_error=-116 stage=read-WHO_AM_I",
            "WHO_AM_I=unavailable (no successful ID read)",
            "health: ready=0 fresh=0 delta_10s=0 stale=0 errors=0 late=1",
        ])
        for line in (path.read_text() if path.exists() else fallback).splitlines():
            self.parser.feed(line, 1_800_000_000_000)
        state = self.parser.snapshot()
        self.assertFalse(state["sensor"]["ready"])
        self.assertIsNone(state["sensor"]["whoAmI"])
        self.assertEqual(state["sensor"]["address"], "0x6b")
        self.assertEqual(state["sensor"]["lastError"], -116)
        self.assertEqual((state["sensor"]["sda"], state["sensor"]["scl"]), (1, 0))
        self.assertEqual(state["sensor"]["sampleHz"], 0)
        self.assertEqual(state["samples"], [])
        self.assertIn("v1.1", state["firmware"])
        self.assertTrue(any(log["level"] == "warn" for log in state["logs"]))

    def test_sensor_id_profiles_and_initialization_are_not_measurements(self):
        for who, profile in [("0x69", "LSM6DS3"), ("0x6a", "LSM6DS3TR-C")]:
            with self.subTest(who=who):
                parser = ImuProtocol()
                parser.feed(f"probe addr=0x6a WHO_AM_I={who} profile={profile} init=0 stage=ready")
                parser.feed(f"IMU ready: profile={profile} addr=0x6a; sensor=104 Hz poll=100 Hz")
                self.assertEqual(parser.sensor["whoAmI"], who)
                self.assertEqual(parser.sensor["profile"], profile)
                self.assertTrue(parser.sensor["ready"])
                self.assertEqual(parser.sensor["fresh"], 0)
                self.assertEqual(parser.sensor["sampleHz"], 0)
                self.assertFalse(parser.samples)

    def test_valid_zero_id_is_distinct_from_unavailable(self):
        self.parser.feed("probe addr=0x6a WHO_AM_I=0x00 profile=unsupported init=-19 stage=unsupported-WHO_AM_I")
        self.assertEqual(self.parser.sensor["whoAmI"], "0x00")
        self.parser.feed("probe addr=0x6b WHO_AM_I=unavailable read_error=-116 stage=read-WHO_AM_I")
        self.assertIsNone(self.parser.sensor["whoAmI"])

    def test_ansi_repeated_shell_prefix_and_host_timestamp(self):
        prefix = "uart:~$ \x1b[8D\x1b[Juart:~$ \x1b[8D\x1b[J[00:00:01.000,123] <inf> nu40_imu: "
        self.assertTrue(self.parser.feed(prefix + sample(42) + "\r\n", 1_800_000_000_123))
        point = self.parser.samples[-1]
        self.assertEqual(point["t"], 1_800_000_000_123)
        self.assertEqual(point["seq"], 42)
        self.assertEqual(point["accel"], [-0.125, 0.25, 0.998])
        self.assertEqual(point["gyro"], [-12.5, 3.75, -0.05])
        self.assertEqual(point["corrected"], [-12.0, 3.5, 0.0])

    def test_sample_success_clears_error_and_preserves_identity(self):
        self.parser.feed("probe addr=0x6a WHO_AM_I=0x69 profile=LSM6DS3 init=0 stage=ready")
        self.parser.feed("Sample failed rc=-116 stage=read-gyro-accel-burst")
        self.parser.feed(sample(51))
        self.assertTrue(self.parser.sensor["ready"])
        self.assertEqual(self.parser.sensor["lastError"], 0)
        self.assertEqual(self.parser.sensor["whoAmI"], "0x69")
        self.assertEqual(self.parser.sensor["fresh"], 51)
        self.assertLess(abs(self.parser.samples[-1]["t"] - time.time() * 1000), 1000)

    def test_malformed_nonfinite_and_overlong_lines_leave_state_untouched(self):
        before = self.parser.snapshot()
        lines = [None, b"hello", "", "uart:~$ imu status", "unrelated", "sample=1",
                 sample().replace("-0.1250", "nan"), sample().replace("-0.1250", "inf"),
                 sample().replace("-0.1250", "1e999"), sample().replace("0.9980", ""),
                 sample() + " junk", sample().replace("sample=1", "sample=-1"),
                 "ready=2 address=0x6a profile=LSM6DS3 last_error=0 stage=ready",
                 "[00:00:01.000,000] <inf> other_module: " + sample(),
                 sample() + " " * 4096]
        for line in lines:
            with self.subTest(line=str(line)[:100]):
                self.assertFalse(self.parser.feed(line, 123))
                self.assertEqual(self.parser.snapshot(), before)

    def test_history_is_bounded_and_snapshots_are_detached(self):
        for seq in range(350):
            self.parser.feed(sample(seq), seq)
        for stamp in range(150):
            self.parser.feed("I2C idle before probe: SDA=1 SCL=1 (expected 1/1)", stamp)
        self.assertEqual(len(self.parser.samples), 300)
        self.assertEqual(self.parser.samples[0]["seq"], 50)
        self.assertEqual(len(self.parser.logs), 100)
        self.assertEqual(self.parser.logs[0]["t"], 50)
        snapshot = self.parser.snapshot()
        snapshot["samples"][0]["accel"][0] = 900
        snapshot["logs"][0]["text"] = "modified"
        snapshot["sensor"]["ready"] = False
        self.assertEqual(self.parser.samples[0]["accel"][0], -0.125)
        self.assertNotEqual(self.parser.logs[0]["text"], "modified")
        self.assertTrue(self.parser.sensor["ready"])

    def test_health_rate_status_counters_and_calibration(self):
        self.parser.feed("health: ready=1 fresh=1234 delta_10s=985 stale=16 errors=2 late=1")
        self.assertEqual(self.parser.sensor["sampleHz"], 98.5)
        self.parser.feed("fresh=1235 stale=17 errors=3 late=2 reconnects=1 age_ms=8")
        self.assertEqual(self.parser.sensor["fresh"], 1235)
        self.assertEqual(self.parser.sensor["stale"], 17)
        self.assertEqual(self.parser.sensor["errors"], 3)
        self.parser.feed("calibrating=0 count=1000 bias_valid=1 bias_dps=-0.010000,0.020000,0.003000 storage=0")
        self.assertTrue(self.parser.sensor["biasValid"])
        self.assertEqual(self.parser.sensor["bias"], [-0.01, 0.02, 0.003])
        self.parser.feed("ready=0 address=0x6b profile=unidentified last_error=-116 stage=read-WHO_AM_I")
        self.assertFalse(self.parser.sensor["ready"])
        self.assertEqual(self.parser.sensor["sampleHz"], 0)
        self.parser.feed("health: ready=0 fresh=1235 delta_10s=15 stale=17 errors=3 late=2")
        self.assertEqual(self.parser.sensor["sampleHz"], 0)

    def test_firmware_status_keeps_detailed_build_identity(self):
        self.parser.feed("NU40-DK IMU v1.1 (Sep 19 2026 16:39:41)")
        self.parser.feed("NU40-DK IMU v1.1: SDA=P0.26 SCL=P0.27 I2C=100000 Hz")
        self.assertEqual(self.parser.firmware, "NU40-DK IMU v1.1 (Sep 19 2026 16:39:41)")
        self.assertIsNone(self.parser.sensor["sda"])
        self.assertIsNone(self.parser.sensor["scl"])

    def test_nu87_actual_banner_module_pins_and_measurement(self):
        prefix = "[00:00:01.000,000] <inf> nu87_imu: "
        lines = [
            "NU87 IMU v1.1 (Oct 10 2026 20:30:00)",
            "NU87 app=0x8002000; SDA=PA26 SCL=PA25 I2C=100000 Hz; UART=1500000 baud",
            "NU87 IMU v1.1: SDA=PA26 SCL=PA25 I2C=100000 Hz",
        ]
        for line in lines:
            self.assertTrue(self.parser.feed(prefix + line, 123))
        self.assertEqual(self.parser.firmware, lines[0])
        self.assertIsNone(self.parser.sensor["sda"])
        self.assertIsNone(self.parser.sensor["scl"])
        self.assertEqual(len(self.parser.samples), 0)
        self.assertTrue(self.parser.feed(prefix + sample(52), 456))
        self.assertEqual(self.parser.samples[-1]["seq"], 52)
        self.assertEqual(self.parser.samples[-1]["t"], 456)
        self.assertTrue(self.parser.sensor["ready"])

    def test_nu87_short_banner_is_not_a_sample(self):
        self.assertTrue(self.parser.feed("NU87 IMU v1.1"))
        self.assertEqual(self.parser.firmware, "NU87 IMU v1.1")
        self.assertFalse(self.parser.sensor["ready"])
        self.assertFalse(self.parser.samples)

    def test_calibration_and_failure_transitions(self):
        self.parser.feed("Calibration load result=-2 bias_valid=0 (missing record is normal)")
        self.assertFalse(self.parser.sensor["biasValid"])
        self.parser.feed("Calibration started: keep sensor still for ~10 seconds")
        self.assertTrue(self.parser.sensor["calibrating"])
        self.parser.feed("Calibration accepted: count=1000 bias_dps=-0.050000,0.020000,0.010000; use imu save")
        self.assertTrue(self.parser.sensor["biasValid"])
        self.assertFalse(self.parser.sensor["calibrating"])
        self.parser.feed(sample())
        self.parser.feed("Sample failed rc=-116 stage=read-STATUS_REG")
        self.assertEqual(self.parser.sensor["lastError"], -116)
        self.parser.feed("Sensor stalled; scheduling reinitialization")
        self.assertFalse(self.parser.sensor["ready"])
        self.assertEqual(len(self.parser.samples), 1)
        self.parser.feed("Calibration clear result=0")
        self.assertFalse(self.parser.sensor["biasValid"])
        self.assertEqual(self.parser.sensor["bias"], [0.0, 0.0, 0.0])


if __name__ == "__main__":
    unittest.main()
