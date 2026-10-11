"""Regression inputs here are test fixtures; the running app has no replay path."""

import math
from pathlib import Path
import tempfile
import threading
import unittest

from voice_tactical.engine import Engine
from voice_tactical.live_imu import LiveImu, valid_packet
from voice_tactical.audio import LocalTranscriber
from voice_tactical.gateway import Gateway
from voice_tactical.server import VoiceApp


def packet(seq=1, tilt=0, uptime=None):
    angle = math.radians(tilt)
    return {
        "configured": True,
        "ready": True,
        "fresh": True,
        "source": "nu87-i2c",
        "sample": seq,
        "ageMs": 2,
        "uptimeMs": seq * 100 if uptime is None else uptime,
        "whoAmI": 0x6A,
        "address": 0x6A,
        "errors": 0,
        "accelMg": [0, round(math.sin(angle) * 1000), round(math.cos(angle) * 1000)],
        "gyroMdps": [30, -20, 10],
    }


class LiveImuTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.engine = Engine(Path(self.temp.name))
        self.time = 0.0
        self.wall_offset = 0.0
        self.imu = LiveImu(
            None, self.engine, enabled=True, clock=lambda: self.time,
            wall_clock=lambda: self.time + self.wall_offset,
        )

    def feed(self, seq, tilt=0, **changes):
        self.time += 0.2
        p = packet(seq, tilt)
        p.update(changes)
        self.imu.feed(p)

    def reference(self):
        self.feed(1)
        self.imu.calibrate()
        for seq in range(2, 16):
            self.feed(seq)
        self.assertIsNotNone(self.imu.reference)

    def test_no_measurement_without_valid_hardware_packet(self):
        s = self.imu.snapshot()
        self.assertIsNone(s["packet"])
        self.assertFalse(s["fresh"])
        self.assertTrue(s["motion_blocked"])
        with self.assertRaises(ValueError):
            self.imu.calibrate()

    def test_invalid_source_identity_numbers_and_old_packet_rejected(self):
        for changes in (
            {"source": "simulation"},
            {"whoAmI": 255},
            {"fresh": False},
            {"sample": True},
            {"ageMs": 501},
            {"uptimeMs": -1},
            {"accelMg": [float("nan"), 0, 1000]},
            {"gyroMdps": [0, 0]},
            {"accelMg": [False, 0, 1000]},
            {"accelMg": [9999, 0, 0]},
        ):
            p = packet()
            p.update(changes)
            self.assertFalse(valid_packet(p), changes)

    def test_duplicate_sequence_does_not_keep_dead_sensor_fresh(self):
        self.feed(1)
        self.time += 2
        self.imu.feed(packet(1))
        self.assertFalse(self.imu.snapshot()["fresh"])
        self.assertIsNone(self.imu.snapshot()["orientation"])

    def test_host_standby_expires_input_when_monotonic_clock_is_paused(self):
        self.reference()
        self.wall_offset += 70 * 60
        state = self.imu.snapshot()
        self.assertFalse(state["fresh"])
        self.assertTrue(state["motion_blocked"])
        self.assertIsNone(state["orientation"])
        self.assertGreaterEqual(state["host_age_ms"], 70 * 60 * 1000)
        # Re-reading the same sensor sequence cannot renew it after resume.
        self.imu.feed(packet(15))
        self.assertFalse(self.imu.snapshot()["fresh"])

    def test_backward_wall_clock_correction_does_not_renew_old_input(self):
        self.feed(1)
        self.wall_offset -= 60
        self.assertFalse(self.imu.snapshot()["fresh"])
        self.feed(2)
        self.assertTrue(self.imu.snapshot()["fresh"])

    def test_new_sample_after_standby_cancels_old_patrol_and_tilt_dwell(self):
        self.reference()
        reference = list(self.imu.reference)
        self.feed(20, 50)
        self.imu.set_virtual_motion("patrol")
        self.wall_offset += 70 * 60
        self.time += 0.7
        self.feed(21, 50)
        self.assertTrue(self.imu.snapshot()["fresh"])
        self.assertFalse(self.imu.latched)
        self.assertEqual(self.imu.reference, reference)
        self.assertEqual(self.imu.virtual_motion, "stop")

    def test_reference_does_not_combine_samples_across_host_standby(self):
        self.feed(1)
        self.imu.calibrate()
        for seq in range(2, 13):
            self.feed(seq)
        self.assertIsNotNone(self.imu.reference_samples)
        self.wall_offset += 70 * 60
        self.feed(13)
        self.assertIsNone(self.imu.reference_samples)
        self.assertIsNone(self.imu.reference)

    def test_raw_axes_preserved_and_gravity_attitude_computed(self):
        self.feed(1, 30)
        s = self.imu.snapshot()
        self.assertEqual(s["packet"]["accelMg"], [0, 500, 866])
        self.assertAlmostEqual(s["orientation"]["roll"], 30, delta=0.05)
        self.assertIsNone(s["orientation"]["absolute_heading"])
        detached = s["packet"]
        detached["sample"] = 99
        self.assertEqual(self.imu.snapshot()["packet"]["sample"], 1)

    def test_reference_uses_stationary_samples_and_persists_real_input(self):
        self.reference()
        self.assertEqual(self.imu.reference, [0, 0, 1])
        records = self.engine.history()
        self.assertEqual(records[0]["kind"], "sensor")
        self.assertEqual(records[0]["sensor_event"]["measurement"]["whoAmI"], 106)
        self.assertEqual(records[0]["sensor_event"]["reference"], [0, 0, 1])

    def test_calibration_rejects_moving_sensor(self):
        self.feed(1)
        self.imu.calibrate()
        self.feed(2, gyroMdps=[20_000, 0, 0])
        self.assertIsNone(self.imu.reference)
        self.assertFalse(self.imu.snapshot()["calibrating"])

    def test_reference_waits_for_twelve_samples_on_a_slow_lan(self):
        self.feed(1)
        self.imu.calibrate()
        for seq in range(2, 13):
            self.time += 0.8
            self.feed(seq)
        self.assertIsNone(self.imu.reference)
        self.assertTrue(self.imu.snapshot()["calibrating"])
        self.time += 0.8
        self.feed(13)
        self.assertEqual(self.imu.reference, [0, 0, 1])

    def test_warning_dwell_latch_return_and_explicit_clear(self):
        self.reference()
        self.feed(20, 50)
        self.feed(21, 50)
        self.assertFalse(self.imu.latched)
        for seq in range(22, 26):
            self.feed(seq, 50)
        self.assertTrue(self.imu.latched)
        self.assertTrue(self.imu.snapshot()["motion_blocked"])
        with self.assertRaises(ValueError):
            self.imu.clear()
        self.feed(26, 0)
        self.assertTrue(self.imu.latched)
        self.imu.clear()
        self.assertFalse(self.imu.latched)
        self.assertEqual(
            [r["sensor_event"]["kind"] for r in self.engine.history()],
            ["clear", "tilt", "reference"],
        )

    def test_interrupted_samples_do_not_count_as_continuous_tilt(self):
        self.reference()
        self.feed(20, 50)
        self.imu.feed({"source": "nu87-i2c", "fresh": False})
        self.time += 2
        self.feed(21, 50)
        self.assertFalse(self.imu.latched)

    def test_high_acceleration_invalidates_attitude_instead_of_claiming_tilt(self):
        self.reference()
        self.feed(20, 50)
        self.feed(21, accelMg=[1900, 0, 0])
        s = self.imu.snapshot()
        self.assertFalse(s["orientation"]["gravity_valid"])
        self.assertTrue(s["motion_blocked"])
        self.assertFalse(s["tilt_latched"])
        self.assertIsNone(self.imu.over_since)
        self.feed(22, 50)
        self.assertFalse(self.imu.latched)

    def test_reboot_requires_reference_again_and_does_not_deadlock(self):
        self.reference()
        self.feed(20, 60)
        for seq in range(21, 26):
            self.feed(seq, 60)
        self.assertTrue(self.imu.latched)
        self.feed(1, uptimeMs=1)
        self.assertIsNone(self.imu.reference)
        self.assertTrue(self.imu.snapshot()["motion_blocked"])
        self.imu.calibrate()
        self.assertTrue(self.imu.snapshot()["calibrating"])

    def test_disabled_module_does_not_block_existing_voice_app(self):
        disabled = LiveImu(None, self.engine)
        disabled.start()
        self.assertIsNone(disabled.worker)
        self.assertIsNone(disabled.poller)
        self.assertFalse(disabled.snapshot()["motion_blocked"])

    def test_slow_oled_ack_does_not_pause_real_input_polling(self):
        output_started = threading.Event()
        release_output = threading.Event()
        input_advanced = threading.Event()

        class SlowOutputGateway:
            sequence = 0

            def imu_status(self):
                self.sequence += 1
                if output_started.is_set() and self.sequence >= 4:
                    input_advanced.set()
                return packet(self.sequence)

            def display(self, text, **kwargs):
                output_started.set()
                release_output.wait(3)
                return {"acknowledged": True}

            def robot_command(self, command):
                return {"accepted": True}

        live = LiveImu(SlowOutputGateway(), self.engine, enabled=True)
        live.start()
        try:
            self.assertTrue(output_started.wait(2), "The output worker must attempt a display.")
            self.assertTrue(input_advanced.wait(2), "Input must advance before the ACK returns.")
            state = live.snapshot()
            self.assertTrue(state["fresh"])
            self.assertGreaterEqual(state["packet"]["sample"], 3)
            self.assertFalse(release_output.is_set())
        finally:
            live.stop.set()
            release_output.set()
            live.worker.join(2)
            live.poller.join(2)

    def test_history_is_bounded(self):
        for seq in range(1, 150):
            self.feed(seq)
        self.assertEqual(len(self.imu.snapshot()["history"]), 100)

    def test_display_filter_preserves_raw_sample_and_warning_threshold(self):
        self.reference()
        self.feed(20, 50)
        s = self.imu.snapshot()
        self.assertAlmostEqual(s["orientation"]["roll"], 50, delta=0.1)
        self.assertLess(s["display_orientation"]["roll"], s["orientation"]["roll"])
        self.assertEqual(s["history"][-1]["accel"], [0, 0.766, 0.643])
        for seq in range(21, 26):
            self.feed(seq, 50)
        self.assertTrue(self.imu.latched)

    def test_fast_presentation_follows_motion_without_changing_raw_warning_evidence(self):
        self.feed(1, 0)
        self.time += 0.1
        self.imu.feed(packet(2, 35))
        state = self.imu.snapshot()
        self.assertAlmostEqual(state["orientation"]["roll"], 35, delta=0.1)
        self.assertGreater(state["display_orientation"]["roll"], 32)
        self.assertEqual(state["packet"]["accelMg"], packet(2, 35)["accelMg"])
        self.assertFalse(state["tilt_latched"])

    def test_presentation_wraps_near_inversion_without_a_full_turn(self):
        self.feed(1, 179)
        self.feed(2, -179)
        state = self.imu.snapshot()
        roll = state["display_orientation"]["roll"]
        self.assertTrue(-180 <= roll <= 180)
        self.assertGreater(abs(roll), 178)
        self.assertTrue(state["fresh"])
        self.assertTrue(state["orientation"]["gravity_valid"])
        for seq in range(3, 16):
            self.feed(seq, 179 if seq % 2 else -179)
            self.assertGreater(abs(self.imu.snapshot()["display_orientation"]["roll"]), 178)

    def test_graphics_updates_new_samples_faster_without_replaying_old_ones(self):
        self.feed(1)
        self.imu.set_display(True, "face")
        self.imu.last_display_state = "LIVE IMU"
        self.imu.last_display = self.time
        self.imu.last_display_sample = 1
        self.assertFalse(self.imu._display_due(self.imu.snapshot(), "LIVE IMU", self.time + 2))
        self.feed(2)
        self.assertFalse(self.imu._display_due(self.imu.snapshot(), "LIVE IMU", 0.45))
        self.assertTrue(self.imu._display_due(self.imu.snapshot(), "LIVE IMU", 0.5))
        self.imu.set_display(True, "text")
        self.imu.last_display_state = "LIVE IMU"
        self.assertFalse(self.imu._display_due(self.imu.snapshot(), "LIVE IMU", 0.5))
        self.assertTrue(self.imu._display_due(self.imu.snapshot(), "IMU LINK STALE", 0.5))

    def test_invalid_packet_cancels_reference_and_view(self):
        self.feed(1)
        self.imu.calibrate()
        self.imu.feed(None)
        s = self.imu.snapshot()
        self.assertFalse(s["calibrating"])
        self.assertIsNone(s["display_orientation"])

    def test_summary_hold_does_not_change_measurement(self):
        self.feed(1)
        before = self.imu.snapshot()["packet"]
        self.imu.hold_display("REPORT SAVED", {"acknowledged": True})
        s = self.imu.snapshot()
        self.assertEqual(s["packet"], before)
        self.assertEqual(s["oled"]["source"], "requested-summary")
        self.assertIsNone(s["oled"]["sample"])

    def test_fresh_then_stale_sensor_blocks_requested_virtual_patrol(self):
        app = VoiceApp(self.engine, LocalTranscriber(), Gateway(), live_imu=True)
        app.live_imu = self.imu
        self.reference()
        self.assertEqual(app.turn(text="훈련구역탐색 시작해.")["motion"], "patrol")
        self.time += 2
        self.assertEqual(app.turn(text="훈련구역탐색 시작해.")["motion"], "stop")

    def test_tilt_warning_blocks_voice_motion_but_allows_report(self):
        app = VoiceApp(self.engine, LocalTranscriber(), Gateway(), live_imu=True)
        app.live_imu = self.imu
        self.reference()
        for seq in range(20, 26):
            self.feed(seq, 50)
        result = app.turn(text="훈련구역탐색 시작해.")
        self.assertEqual(result["motion"], "stop")
        result = app.turn(text="상황 보고. 위치: 훈련장. 관측: 보드 기울기 경고.", mode="report")
        self.assertEqual(result["kind"], "report")
        self.assertIsNone(result["motion"])


if __name__ == "__main__":
    unittest.main()
