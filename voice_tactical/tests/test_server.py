"""Exercise the real HTTP boundaries and a persisted report end to end."""
import http.client
import json
from pathlib import Path
import tempfile
import threading
import unittest

from voice_tactical.engine import Engine
from voice_tactical.audio import LocalTranscriber
from voice_tactical.gateway import Gateway
from voice_tactical.server import VoiceApp, create_server


class ServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.app = VoiceApp(Engine(Path(cls.temp.name)), LocalTranscriber(), Gateway())
        cls.server = create_server(cls.app, 0)
        cls.port = cls.server.server_address[1]
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=3)
        cls.temp.cleanup()

    def request(self, method, path, body=None, headers=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            conn.request(method, path, body, headers or {})
            response = conn.getresponse()
            return response.status, response.read(), dict(response.getheaders())
        finally:
            conn.close()

    def post(self, value, **headers):
        return self.request("POST", "/api/turn", json.dumps(value), {"Content-Type": "application/json", **headers})

    def test_report_is_saved_and_searchable(self):
        status, body, _ = self.post({"text": "상황 보고. 보고자: 지원팀. 위치: 창고. 관측: 통신 장비 점검 완료.", "mode": "report"})
        self.assertEqual(status, 200, body)
        result = json.loads(body)
        self.assertEqual(result["kind"], "report")
        self.assertIsNone(result["motion"])
        status, body, _ = self.request("GET", "/api/records")
        self.assertEqual(status, 200)
        self.assertTrue(any(row["id"] == result["id"] for row in json.loads(body)["records"]))

    def test_status_is_honest_without_model_or_hardware(self):
        status, body, _ = self.request("GET", "/api/status")
        self.assertEqual(status, 200)
        state = json.loads(body)
        self.assertFalse(state["stt"]["ready"])
        self.assertFalse(state["gateway"]["configured"])
        self.assertFalse(state["capabilities"]["kit_ota"])

    def test_cross_origin_and_rebound_host_are_rejected(self):
        for headers in ({"Origin": "https://example.com"}, {"Origin": "null"}, {"Host": "example.com"}, {"Sec-Fetch-Site": "cross-site"}):
            with self.subTest(headers=headers):
                self.assertEqual(self.post({"text": "왈라비 정지"}, **headers)[0], 403)

    def test_non_json_and_invalid_modes_do_not_execute(self):
        self.assertEqual(self.request("POST", "/api/turn", '{"text":"왈라비 앞으로"}', {"Content-Type": "text/plain"})[0], 415)
        for value in ({"text": ""}, {"text": 42}, {"text": "a" * 8001}, {"text": "왈라비 앞으로", "mode": "bad"}):
            self.assertEqual(self.post(value)[0], 400)

    def test_one_active_turn_and_body_limits(self):
        self.app.turn_lock.acquire()
        try:
            self.assertEqual(self.post({"text": "상황 보고: 장비 점검 완료."})[0], 409)
            self.assertEqual(self.post({"text": "정지"})[0], 200)
        finally:
            self.app.turn_lock.release()
        status, _, _ = self.request("POST", "/api/audio", b"a", {"Content-Type": "audio/wav", "Content-Length": "9999999"})
        self.assertEqual(status, 413)

    def test_stop_and_alert_status_work_without_microphone(self):
        status, body, _ = self.request("POST", "/api/control/stop", "{}", {"Content-Type": "application/json"})
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(body)["stopped"])
        status, body, _ = self.request("GET", "/api/alerts")
        self.assertEqual(status, 200)
        self.assertFalse(json.loads(body)["device"]["available"])
        self.assertEqual(self.request("GET", "/api/device-audio/status?generation=bad")[0], 400)

    def test_private_assets_are_never_served(self):
        for path in ("/../server.py", "/%2e%2e/server.py", "/data/records.sqlite3", "/simulation/serve.py", "/api/missing"):
            self.assertEqual(self.request("GET", path)[0], 404)

    def test_offline_assets_and_no_cors(self):
        for path in ("/", "/hardware.html", "/hardware.mjs", "/hardware.css", "/oled-preview.mjs", "/simulation/",
                     "/simulation/model.mjs", "/simulation/attitude-view.mjs",
                     "/simulation/live-imu-control.mjs", "/demo/", "/demo/player.mjs"):
            status, body, headers = self.request("GET", path)
            self.assertEqual(status, 200, path)
            self.assertTrue(body)
            self.assertNotIn("Access-Control-Allow-Origin", headers)
            self.assertIn("default-src 'self'", headers["Content-Security-Policy"])

    def test_imu_controls_validate_input_and_do_not_create_samples(self):
        status, body, _ = self.request("GET", "/api/imu/status")
        self.assertEqual(status, 200)
        self.assertFalse(json.loads(body)["fresh"])
        for path in ("/api/imu/reference", "/api/imu/clear"):
            self.assertEqual(self.request("POST", path, "{}",
                                         {"Content-Type": "application/json"})[0], 400)
        status, _, _ = self.request("POST", "/api/imu/display", '{"enabled":"true"}',
                                    {"Content-Type": "application/json"})
        self.assertEqual(status, 400)
        self.assertIsNone(self.app.live_imu.snapshot()["packet"])
        self.assertEqual(self.request("POST", "/api/imu/reference", "{}",
                                     {"Content-Type": "application/json",
                                      "Origin": "https://example.com"})[0], 403)

    def test_demo_serves_only_browser_assets(self):
        for path in ("/demo/record.mjs", "/demo/voice-check.json", "/demo/README.md",
                     "/demo/%2e%2e/server.py", "/demo/../military-wallaby-report.md"):
            self.assertEqual(self.request("GET", path)[0], 404, path)


if __name__ == "__main__":
    unittest.main()
