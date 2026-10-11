import unittest
from pathlib import Path
import tempfile

from voice_tactical.gateway import _validate_text
from voice_tactical.oled_scene import scene_command
from voice_tactical.engine import Engine
from voice_tactical.live_imu import LiveImu


class OledSceneTests(unittest.TestCase):
    def state(self, **changes):
        return {"fresh": True, "display_orientation": {
            "roll": -12.4, "pitch": 10.6, "tilt": 16, "gravity_valid": True},
            "packet": {"sample": 202748}, "reference": [0, 0, 1], **changes}

    def test_real_pose_and_sequence_fit_existing_acknowledged_text_protocol(self):
        wire, scene = scene_command(self.state(), "horizon")
        self.assertEqual(wire, "@WG1 H L\n-12 11 16\n202748")
        self.assertEqual(scene["source"], "nu87-i2c")
        self.assertTrue(scene["attitude_valid"])
        _validate_text(wire)

    def test_warning_priority_and_stale_do_not_invent_angles(self):
        for changes, expected in (({"tilt_latched": True}, "A"),
                                  ({"fresh": False}, "X"),
                                  ({"calibrating": True}, "C"),
                                  ({"reference": None}, "R")):
            wire, scene = scene_command(self.state(**changes), "face", "patrol")
            self.assertEqual(scene["state"], expected)
            _validate_text(wire)
        _, scene = scene_command(self.state(fresh=False, tilt_latched=True), "face")
        self.assertEqual(scene["state"], "A")
        self.assertFalse(scene["attitude_valid"])
        self.assertEqual(scene["roll"], 0)

    def test_virtual_motion_has_explicit_scope(self):
        _, scene = scene_command(self.state(), "face", "patrol")
        self.assertEqual(scene["state"], "P")
        self.assertEqual(scene["motion"], "virtual-patrol-request")

    def test_moving_gravity_estimate_is_distinct_from_stale_transport(self):
        state = self.state(display_orientation=None)
        wire, scene = scene_command(state, "face")
        self.assertEqual(scene["state"], "U")
        self.assertFalse(scene["attitude_valid"])
        self.assertEqual((scene["roll"], scene["pitch"], scene["tilt"]), (0, 0, 0))
        _validate_text(wire)
        _, stale = scene_command({**state, "fresh": False}, "face")
        self.assertEqual(stale["state"], "X")
        _, alarm = scene_command({**state, "alarm_active": True}, "face")
        self.assertEqual(alarm["state"], "B")

    def test_maximum_fields_wrap_and_invalid_input_rejected(self):
        s = self.state(packet={"sample": 0xffffffff}, display_orientation={
            "roll": 540, "pitch": -90, "tilt": 180, "gravity_valid": True})
        wire, scene = scene_command(s, "horizon")
        self.assertEqual(scene["roll"], -180)
        _validate_text(wire)
        for sample in (True, -1, 0x100000000):
            with self.assertRaises(ValueError):
                scene_command(self.state(packet={"sample": sample}), "face")
        with self.assertRaises(ValueError):
            scene_command(self.state(), "video")


class OledSettingsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.imu = LiveImu(None, Engine(Path(temporary.name)), enabled=True, clock=lambda: 10)
    def test_settings_validate_before_mutating_and_cancel_text_hold(self):
        self.imu.hold_display("REPORT SAVED", {})
        result = self.imu.set_display(True, "horizon")
        self.assertEqual(result["display_view"], "horizon")
        self.assertEqual(self.imu.display_hold_until, 0)
        for enabled, view in ((1, "face"), (True, "unknown")):
            with self.assertRaises(ValueError):
                self.imu.set_display(enabled, view)
        self.assertEqual(self.imu.snapshot()["display_view"], "horizon")
