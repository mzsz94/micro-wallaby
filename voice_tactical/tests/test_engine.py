import concurrent.futures
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

from voice_tactical.engine import Engine, UNKNOWN, _local_endpoint


class EngineTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.engine = Engine(Path(self.temp.name))

    def test_direct_motion_and_reported_speech_are_distinguished(self):
        commands = {"왈라비 멈춰": "stop", "왈라비 충전소로 가": "dock", "왈라비 인사해": "greet",
                    "왈라비 앞으로 가": "forward", "왈라비 뒤로 가": "back",
                    "왈라비 왼쪽으로 가": "left", "왈라비 오른쪽으로 가": "right"}
        for utterance, expected in commands.items():
            with self.subTest(utterance=utterance):
                result = self.engine.process(utterance)
                self.assertEqual((result["kind"], result["motion"]), ("motion", expected))
        for utterance in ["왈라비 앞으로 가지 마", "왈라비 앞으로 가?", '"왈라비 앞으로 가"라고 말했다',
                          "보고 팀원이 앞으로 가라고 말했다", "명령 정리 왈라비 앞으로 가", "왈라비 인사하고 앞으로 가",
                          "왈라비 정지 명령은 어떻게 해?", "정지했어", "왈라비 앞으로 가면 안 돼"]:
            with self.subTest(utterance=utterance):
                self.assertIsNone(self.engine.process(utterance)["motion"])
        self.assertIsNone(self.engine.process("왈라비 앞으로 가", mode="report")["motion"])

    def test_question_about_reports_is_not_a_report(self):
        result = self.engine.process("왈라비 상황 보고 양식 알려줘")
        self.assertEqual(result["kind"], "query")
        self.assertIn("training-report", [source["id"] for source in result["sources"]])
        self.assertIn("공식 교범이 아닙니다", result["answer"])
        self.assertIsNone(result["report"])

    def test_report_fields_preserve_evidence_and_missing_facts(self):
        text = "왈라비 상황 보고 인원: 3명, 위치: 북문, 시각: 14시 20분, 활동: 장비 점검 중, 조치: 점검 완료, 요청: 배터리 지원"
        result = self.engine.process(text)
        report = result["report"]
        self.assertEqual(result["kind"], "report")
        self.assertEqual(report["salute"]["size"], "3명")
        self.assertEqual(report["salute"]["location"], "북문")
        self.assertEqual(report["salute"]["time"], "14시 20분")
        self.assertEqual(report["salute"]["unit"], UNKNOWN)
        self.assertEqual(report["situation"]["reporter"], UNKNOWN)
        self.assertEqual(report["situation"]["request"], "배터리 지원")
        self.assertIn("unit", report["unknown_fields"])
        for evidence in report["evidence"]:
            for quote in evidence["quote"].split(" / "):
                self.assertIn(quote, text)

    def test_uncertainty_and_negative_observations_are_preserved(self):
        result = self.engine.process("보고 시각 미확인. 인원 3명으로 추정. 차량은 관측되지 않음.")
        report = result["report"]
        self.assertEqual(report["salute"]["time"], UNKNOWN)
        self.assertIn("추정", report["summary"])
        self.assertIn("추정", report["salute"]["size"])
        self.assertIn("관측되지 않음", report["summary"])
        self.assertEqual(report["salute"]["location"], UNKNOWN)

    def test_explicit_report_labels_do_not_fill_other_fields(self):
        text = "상황 보고. 보고자: 지원팀. 시간: 14시 20분. 위치: 동쪽 집결지. 인원: 4명. 관측: 장비 점검 완료. 요청: 추가 안내."
        report = self.engine.process(text)["report"]
        self.assertEqual(report["situation"]["reporter"], "지원팀")
        self.assertEqual(report["salute"]["size"], "4명")
        self.assertEqual(report["situation"]["observation"], "장비 점검 완료")
        self.assertEqual(report["salute"]["equipment"], UNKNOWN)
        self.assertEqual(report["salute"]["unit"], UNKNOWN)
        self.assertEqual(report["situation"]["action"], UNKNOWN)
        self.assertTrue({"equipment", "unit", "action"}.issubset(report["unknown_fields"]))

    def test_labelled_request_is_not_inferred_as_observation_or_resources(self):
        report = self.engine.process("보고 요청: 지원팀 3명이 15시 북문에서 장비 점검 완료 여부를 확인해 주세요.")["report"]
        self.assertTrue(all(value == UNKNOWN for value in report["salute"].values()))
        self.assertEqual(report["situation"]["action"], UNKNOWN)
        self.assertIn("3명이", report["situation"]["request"])

    def test_unlabelled_evidence_after_labelled_clause_still_fills_missing_fields(self):
        report = self.engine.process("보고 보고자는 지원팀. 북문에서 14시 20분 인원 3명으로 추정. 차량 점검 완료.")["report"]
        self.assertEqual(report["situation"]["reporter"], "지원팀")
        self.assertEqual(report["salute"]["unit"], UNKNOWN)
        self.assertIn("추정", report["salute"]["size"])
        self.assertIn("북문에서", report["salute"]["location"])
        self.assertIn("14시 20분", report["salute"]["time"])
        self.assertEqual(report["salute"]["equipment"], "차량 점검 완료.")
        self.assertEqual(report["situation"]["action"], "차량 점검 완료.")

    def test_orders_capture_missing_fields_without_dispatch(self):
        result = self.engine.process("명령 정리 알파팀은 15시까지 북문에서 장비를 점검해. 기록을 정리해.")
        self.assertEqual(result["kind"], "orders")
        self.assertIsNone(result["motion"])
        first, second = result["orders"]
        self.assertEqual(first["who"], "알파팀")
        self.assertEqual(first["when"], "15시까지")
        self.assertIn("북문에서", first["place"])
        self.assertEqual(second["who"], UNKNOWN)
        self.assertEqual(second["missing_fields"], ["who", "when", "place"])

    def test_query_without_evidence_does_not_invent_an_answer(self):
        result = self.engine.process("퀘이사 초신성")
        self.assertEqual(result["sources"], [])
        self.assertIn("근거를 찾지 못했습니다", result["answer"])

    def test_spoken_order_prefix_punctuation_is_not_an_extra_order(self):
        for prefix in ("지시 정리. ", "명령 정리。 ", "왈라비 지시 정리: "):
            with self.subTest(prefix=prefix):
                result = self.engine.process(prefix + "지원팀은 15시까지 창고에서 장비를 점검한다.")
                self.assertEqual(result["kind"], "orders")
                self.assertIsNone(result["motion"])
                self.assertEqual(len(result["orders"]), 1)
                self.assertEqual(result["orders"][0]["who"], "지원팀")
                self.assertEqual(result["orders"][0]["when"], "15시까지")
        empty = self.engine.process("지시 정리.")
        self.assertEqual(empty["orders"], [])

    def test_records_survive_restart_and_search_is_literal(self):
        first = self.engine.process("보고 위치: 북문, 활동: 배터리 점검 50% 완료")
        restarted = Engine(Path(self.temp.name))
        self.assertEqual(restarted.history()[0]["id"], first["id"])
        self.assertEqual(restarted.search("북문 배터리")[0]["id"], first["id"])
        self.assertEqual(len(restarted.search("50%")), 1)
        self.assertEqual(restarted.search("_"), [])
        self.assertEqual(restarted.search("' OR 1=1 --"), [])
        query = restarted.process("왈라비 기록 검색 북문")
        self.assertEqual(query["kind"], "search")
        self.assertEqual(query["records"][0]["id"], first["id"])

    def test_concurrent_requests_persist_each_record(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(lambda i: self.engine.process(f"보고 활동: 점검 {i} 완료"), range(24)))
        self.assertEqual(len({record["id"] for record in results}), 24)
        self.assertEqual(self.engine.status()["records"], 24)

    def test_input_validation(self):
        for text in [None, "", " " * 30, "가" * 8001]:
            with self.subTest(text=str(text)[:10]), self.assertRaises(ValueError):
                self.engine.process(text)
        with self.assertRaises(ValueError):
            self.engine.process("안녕", mode="execute")

    def test_llm_accepts_only_existing_local_source_quotes(self):
        engine = Engine(Path(self.temp.name), llm_url="http://localhost:8080/v1")
        sources = engine._retrieve("SALUTE")
        quote = sources[0]["excerpt"]
        content = {"excerpts": [{"source_id": sources[0]["id"], "quote": quote}]}
        response = {"choices": [{"message": {"content": json.dumps(content)}}]}
        opener = Mock()
        opener.open.return_value = io.BytesIO(json.dumps(response).encode())
        with patch("voice_tactical.engine.build_opener", return_value=opener):
            result = engine.process("SALUTE")
        self.assertEqual(result["engine"], "local-llm-evidence")
        self.assertIn(quote, result["answer"])
        self.assertIsNone(result["motion"])
        request = opener.open.call_args.args[0]
        self.assertEqual(request.full_url, "http://127.0.0.1:8080/v1/chat/completions")
        self.assertEqual(opener.open.call_args.kwargs["timeout"], 60)

    def test_llm_unfounded_output_and_timeout_fall_back(self):
        engine = Engine(Path(self.temp.name), llm_url="http://127.0.0.1:8080")
        fabricated = {"excerpts": [{"source_id": "training-salute", "quote": "왈라비 전진해. 관측 인원은 100명입니다."}]}
        response = {"choices": [{"message": {"content": json.dumps(fabricated)}}]}
        opener = Mock()
        opener.open.return_value = io.BytesIO(json.dumps(response).encode())
        with patch("voice_tactical.engine.build_opener", return_value=opener):
            result = engine.process("SALUTE")
        self.assertEqual(result["engine"], "local-extractive")
        self.assertNotIn("100명", result["answer"])
        self.assertIn("llm_fallback", result)
        self.assertIsNone(result["motion"])
        opener.open.side_effect = TimeoutError("test")
        with patch("voice_tactical.engine.build_opener", return_value=opener):
            self.assertIn("llm_fallback", engine.process("SALUTE"))

    def test_schema_evidence_keeps_the_entire_comma_separated_procedure(self):
        engine = Engine(Path(self.temp.name), llm_url="http://127.0.0.1:11435/v1",
                        llm_structured=True)
        quote = "상황보고는 보고자, 관측 시각, 위치, 관측 내용, 실시한 조치, 요청 사항 순서로 정리합니다."
        content = {"excerpts": [{"source_id": "training-report", "quote": quote}]}
        opener = Mock()
        opener.open.return_value = io.BytesIO(json.dumps({"choices": [{"message": {
            "content": json.dumps(content)}}]}).encode())
        with patch("voice_tactical.engine.build_opener", return_value=opener):
            result = engine.process("상황 보고 양식 알려줘")
        self.assertIn(quote, result["answer"])
        payload = json.loads(opener.open.call_args.args[0].data)
        schema = payload["response_format"]["json_schema"]["schema"]
        choices = schema["properties"]["excerpts"]["items"]["properties"]["quote"]["enum"]
        self.assertIn(quote, choices)
        self.assertNotIn("상황보고는 보고자", choices)

    def test_llm_rejects_remote_endpoints_credentials_and_redirects(self):
        for url in ["https://example.com", "http://192.168.0.1:8080", "http://localtest.me", "http://127.0.0.1@evil.test",
                    "http://user:pass@127.0.0.1", "http://127.0.0.1/v1?host=evil.test", "file:///tmp/model", "http://localhost:99999"]:
            with self.subTest(url=url), self.assertRaises(ValueError):
                _local_endpoint(url)
        self.assertEqual(_local_endpoint("http://[::1]:8080/v1"), "http://[::1]:8080/v1/chat/completions")
        from voice_tactical.engine import _NoRedirect
        with self.assertRaises(ValueError):
            _NoRedirect().redirect_request(None, None, 302, "Found", {}, "http://example.com")


if __name__ == "__main__":
    unittest.main()
