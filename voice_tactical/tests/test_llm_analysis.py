import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

from voice_tactical.engine import Engine


def response(value):
    raw = json.dumps({"choices": [{"message": {"content": json.dumps(value, ensure_ascii=False)}}]}).encode()
    return io.BytesIO(raw)


class LocalAnalysisTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.engine = Engine(Path(self.folder.name), llm_url="http://127.0.0.1:11435/v1", llm_model="local-test")

    def process(self, text, value):
        opener = Mock()
        opener.open.return_value = response(value)
        with patch("voice_tactical.engine.build_opener", return_value=opener):
            return self.engine.process(text)

    def test_report_analysis_uses_verified_source_spans(self):
        result = self.process("상황 보고. 위치: 창고. 관측: 장비 점검 완료. 요청: 추가 안내.", {
            "kind": "report", "motion": None, "summary_quotes": ["장비 점검 완료", "추가 안내"],
            "fields": {"location": "창고", "activity": "장비 점검 완료", "request": "추가 안내"}})
        self.assertEqual(result["engine"], "local-llm-analysis")
        self.assertIn("장비 점검 완료", result["report"]["summary"])
        self.assertEqual(result["report"]["situation"]["location"], "창고")
        self.assertIsNone(result["motion"])

    def test_hallucination_or_lost_uncertainty_falls_back(self):
        for fields in ({"location": "다른 장소"}, {"size": "3명"}):
            result = self.process("상황 보고. 인원: 3명으로 추정. 위치: 창고.", {
                "kind": "report", "motion": None, "summary_quotes": [], "fields": fields})
            self.assertIn("llm_fallback", result)
            self.assertIn("추정", result["report"]["salute"]["size"])
        result = self.process("상황 보고. 관측: 이상 없음으로 추정.", {
            "kind": "report", "motion": None, "summary_quotes": [],
            "fields": {"activity": "이상 없음"}})
        self.assertIn("llm_fallback", result)
        self.assertIn("추정", result["report"]["salute"]["activity"])

    def test_llm_cannot_replace_allowlisted_motion(self):
        result = self.process("왈라비 앞으로 가", {"kind": "motion", "motion": "dock", "summary_quotes": [], "fields": {}})
        self.assertIn("llm_fallback", result)
        self.assertEqual(result["motion"], "forward")
        result = self.process("왈라비 앞으로 가", {"kind": "motion", "motion": "forward", "summary_quotes": [], "fields": {}})
        self.assertEqual(result["engine"], "local-llm-analysis")
        self.assertEqual(result["motion"], "forward")

    def test_summary_keeps_uncertainty_and_negation_in_source_clause(self):
        for text, kind, quote, qualification in (
            ("상황 보고. 인원 3명으로 추정됩니다.", "report", "인원 3명", "추정"),
            ("명령 정리. 창고로 이동하지 않는다.", "orders", "창고로 이동", "않는다"),
        ):
            with self.subTest(kind=kind):
                result = self.process(text, {"kind": kind, "motion": None,
                    "summary_quotes": [quote], "fields": {}})
                self.assertEqual(result["engine"], "local-llm-analysis")
                self.assertIn(qualification, result["answer"])
                self.assertIn(qualification, result["analysis"]["summary_quotes"][0])
                self.assertIsNone(result["motion"])

    def test_emergency_paths_do_not_wait_for_llm(self):
        with patch.object(self.engine, "_llm_analysis") as call:
            self.assertEqual(self.engine.process("왈라비 정지")["motion"], "stop")
            self.assertEqual(self.engine.process("훈련구역탐색 시작해.")["motion"], "patrol")
            self.assertEqual(self.engine.process("탐색 중지")["motion"], "stop")
            self.assertEqual(self.engine.record_device_alert(True, 3)["motion"], "stop")
            call.assert_not_called()

    def test_multi_clause_summary_is_still_rejected(self):
        text = "상황 보고. 위치: 창고. 관측: 장비 점검 완료. 요청: 추가 안내."
        result = self.process(text, {"kind": "report", "motion": None,
            "summary_quotes": [text], "fields": {"location": "창고"}})
        self.assertIn("llm_fallback", result)
        self.assertEqual(result["engine"], "local-extractive")
        self.assertEqual(result["report"]["salute"]["location"], "창고")

    def test_schema_mode_keeps_independent_source_validation(self):
        self.engine.llm_structured = True
        text = "상황 보고. 위치: 창고. 인원: 3명으로 추정."
        opener = Mock()
        opener.open.return_value = response({"kind": "report", "motion": None,
            "summary_quotes": ["인원: 3명으로 추정."], "fields": {"size": "3명"}})
        with patch("voice_tactical.engine.build_opener", return_value=opener):
            result = self.engine.process(text)
        self.assertIn("llm_fallback", result)
        self.assertIn("추정", result["report"]["salute"]["size"])
        request = json.loads(opener.open.call_args.args[0].data)
        self.assertEqual(request["response_format"]["type"], "json_schema")
        schema = request["response_format"]["json_schema"]["schema"]
        self.assertEqual(schema["properties"]["summary_quotes"]["maxItems"], 3)
        self.assertNotIn(text, schema["properties"]["summary_quotes"]["items"]["enum"])

    def test_basic_mode_preserves_json_object_server_compatibility(self):
        self.assertEqual(self.engine._llm_format("unused", {}), {"type": "json_object"})

    def test_summary_retains_observation_when_model_omits_that_field(self):
        result = self.process("상황 보고. 위치: 창고. 관측: 장비 점검 완료. 요청: 추가 안내.", {
            "kind": "report", "motion": None, "summary_quotes": ["위치: 창고."],
            "fields": {"location": "창고", "request": "추가 안내"}})
        self.assertEqual(result["engine"], "local-llm-analysis")
        self.assertIn("장비 점검 완료", result["answer"])
        self.assertIn("추가 안내", result["answer"])


if __name__ == "__main__":
    unittest.main()
