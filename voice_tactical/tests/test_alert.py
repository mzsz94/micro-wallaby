from pathlib import Path
import tempfile
import unittest

from voice_tactical.engine import Engine


class AlertTests(unittest.TestCase):
    def test_detection_latches_stop_persists_and_explicit_clear_allows_new_motion(self):
        with tempfile.TemporaryDirectory() as folder:
            engine = Engine(Path(folder))
            result = engine.process("왈라비 폭발물 감지")
            self.assertEqual(result["kind"], "alert")
            self.assertEqual(result["motion"], "stop")
            self.assertTrue(result["alert"]["active"])
            self.assertTrue(result["alert"]["simulated"])
            engine = Engine(Path(folder))
            self.assertTrue(engine.status()["alert_active"])
            self.assertIsNone(engine.process("왈라비 앞으로 가")["motion"])
            self.assertEqual(engine.process("왈라비 정지")["motion"], "stop")
            self.assertFalse(engine.process("왈라비 경고 해제")["alert"]["active"])
            self.assertEqual(engine.process("왈라비 앞으로 가")["motion"], "forward")

    def test_negative_question_report_and_quote_do_not_sound_alarm(self):
        with tempfile.TemporaryDirectory() as folder:
            engine = Engine(Path(folder))
            for text in ("폭발물 감지 안 됐어", "폭발물 감지 방법은?", "상황 보고. 폭발물 감지라는 말을 들었다", '"폭발물 감지"라고 말했다'):
                self.assertNotEqual(engine.process(text)["kind"], "alert", text)
            self.assertNotEqual(engine.process("왈라비 폭발물 감지", mode="report")["kind"], "alert")
            self.assertFalse(engine.alert_active)


if __name__ == "__main__":
    unittest.main()
