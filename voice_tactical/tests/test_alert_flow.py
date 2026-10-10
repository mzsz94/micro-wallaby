from pathlib import Path
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from voice_tactical.engine import Engine
from voice_tactical.server import VoiceApp


class AlarmGateway:
    configured = True

    def __init__(self):
        self.active = False
        self.counter = 0
        self.source = "none"
        self.fail_clear = False
        self.fail_robot = False
        self.alarm_calls = []
        self.robot_calls = []

    def alarm_status(self):
        return {"ok": True, "available": True, "active": self.active, "acknowledged": True,
                "eventCounter": self.counter, "source": self.source, "generation": 1}

    def alarm(self, active):
        self.alarm_calls.append(active)
        if not active and self.fail_clear:
            return {"ok": False, "acknowledged": False, "error": "disconnected"}
        self.active = active
        self.source = "command"
        return {"ok": True, "available": True, "acknowledged": True, "active": active}

    def robot_command(self, command):
        self.robot_calls.append(command)
        return {"ok": not self.fail_robot, "accepted": not self.fail_robot, "command": command, "state": "idle", "simulated": True}

    def status(self):
        return {"configured": self.configured}


class AlertFlowTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.engine = Engine(Path(self.temp.name))
        self.gateway = AlarmGateway()
        self.app = VoiceApp(self.engine, SimpleNamespace(status=lambda: {"ready": False}), self.gateway)

    def tearDown(self):
        self.temp.cleanup()

    def test_btn4_records_once_even_if_later_ack_changes_source(self):
        self.gateway.active, self.gateway.counter, self.gateway.source = True, 1, "command"
        self.app.turn_lock.acquire()
        try:
            first = self.app.alerts()
            second = self.app.alerts()
        finally:
            self.app.turn_lock.release()
        self.assertTrue(first["active"])
        self.assertEqual(first["last_result"]["alert"]["source"], "nu40-btn4")
        self.assertEqual(first["last_result"]["id"], second["last_result"]["id"])
        self.assertEqual(len(self.engine.history()), 1)

    def test_btn4_during_model_inference_blocks_delayed_movement(self):
        entered, release = threading.Event(), threading.Event()
        original = self.engine.process
        response = []

        def process(text, *args, **kwargs):
            if text == "왈라비 앞으로":
                entered.set()
                release.wait(2)
                return {"id": "delayed", "kind": "motion", "motion": "forward", "answer": "전진", "transcript": text}
            return original(text, *args, **kwargs)

        with patch.object(self.engine, "process", side_effect=process):
            worker = threading.Thread(target=lambda: response.append(self.app.turn(text="왈라비 앞으로")))
            worker.start()
            self.assertTrue(entered.wait(1))
            self.gateway.active, self.gateway.counter, self.gateway.source = True, 1, "button"
            immediate = self.app.alerts()
            self.assertTrue(immediate["active"])
            self.assertTrue(self.engine.alert_active)
            release.set()
            worker.join(2)
        self.assertFalse(worker.is_alive())
        self.assertEqual(response[0]["motion"], "stop")
        self.assertTrue(response[0]["alert_active"])
        self.assertNotIn("forward", self.gateway.robot_calls)

    def test_failed_clear_preserves_persisted_latch(self):
        self.app.turn(text="왈라비 폭발물 감지")
        self.gateway.fail_clear = True
        result = self.app.turn(text="왈라비 경고 해제")
        self.assertTrue(result["alert"]["active"])
        self.assertTrue(result["alert"]["clear_requested"])
        self.assertTrue(self.app.alert_active)
        self.assertTrue(Engine(Path(self.temp.name)).alert_active)
        self.assertEqual(result["motion"], "stop")

    def test_confirmed_clear_releases_latch_and_allows_robot_command(self):
        self.app.turn(text="왈라비 폭발물 감지")
        result = self.app.turn(text="왈라비 경고 해제")
        self.assertFalse(result["alert"]["active"])
        self.assertFalse(self.app.alerts()["active"])
        result = self.app.turn(text="왈라비 앞으로")
        self.assertEqual(result["motion"], "forward")
        self.assertTrue(result["robot"]["accepted"])

    def test_configured_robot_rejection_stops_simulation(self):
        self.gateway.fail_robot = True
        result = self.app.turn(text="왈라비 앞으로")
        self.assertEqual(result["motion"], "stop")
        self.assertFalse(result["robot"]["accepted"])

    def test_global_stop_invalidates_inference_before_robot_dispatch(self):
        entered, release = threading.Event(), threading.Event()
        response = []
        original = self.engine.process

        def process(text, *args, **kwargs):
            if text == "왈라비 앞으로":
                entered.set()
                release.wait(2)
                return {"id": "cancelled", "kind": "motion", "motion": "forward", "answer": "전진", "transcript": text}
            return original(text, *args, **kwargs)

        with patch.object(self.engine, "process", side_effect=process):
            worker = threading.Thread(target=lambda: response.append(self.app.turn(text="왈라비 앞으로")))
            worker.start()
            self.assertTrue(entered.wait(1))
            self.app.stop_motion()
            release.set()
            worker.join(2)
        self.assertFalse(worker.is_alive())
        self.assertEqual(response[0]["motion"], "stop")
        self.assertTrue(response[0]["motion_cancelled"])
        self.assertNotIn("forward", self.gateway.robot_calls)

    def test_software_emergency_does_not_wait_for_inference_lock(self):
        self.app.turn_lock.acquire()
        try:
            alarm = self.app.turn(text="왈라비 폭발물 감지")
            self.assertTrue(alarm["alert"]["active"])
            cleared = self.app.turn(text="왈라비 경고 해제")
            self.assertFalse(cleared["alert"]["active"])
            self.assertTrue(self.app.turn_lock.locked())
        finally:
            self.app.turn_lock.release()

    def test_no_gateway_explicitly_uses_local_simulation(self):
        self.gateway.configured = False
        result = self.app.turn(text="왈라비 앞으로")
        self.assertEqual(result["motion"], "forward")
        self.assertEqual(result["robot"]["mode"], "local-simulation")
        self.assertEqual(self.gateway.robot_calls, [])

    def test_reconnect_reassertion_finishes_before_explicit_clear(self):
        self.app.turn(text="왈라비 폭발물 감지")
        self.gateway.active = False
        entered, release = threading.Event(), threading.Event()
        original = self.gateway.alarm

        def alarm(active):
            if active:
                entered.set()
                release.wait(2)
            return original(active)

        with patch.object(self.gateway, "alarm", side_effect=alarm):
            poll = threading.Thread(target=self.app.alerts)
            poll.start()
            self.assertTrue(entered.wait(1))
            clear = threading.Thread(target=lambda: self.app.turn(text="왈라비 경고 해제"))
            clear.start()
            release.set()
            poll.join(2)
            clear.join(2)
        self.assertFalse(poll.is_alive())
        self.assertFalse(clear.is_alive())
        self.assertEqual(self.gateway.alarm_calls[-2:], [True, False])
        self.assertFalse(self.app.alert_active)
        self.assertFalse(self.gateway.active)


if __name__ == "__main__":
    unittest.main()
