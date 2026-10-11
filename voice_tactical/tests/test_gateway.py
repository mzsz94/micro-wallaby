"""Exercise the gateway's actual HTTP adapter with a bounded fake transport."""
import json
import os
from pathlib import Path
import socket
import tempfile
import threading
import unittest
from unittest.mock import patch

from voice_tactical.gateway import Gateway, KIT_PROTOCOL, summarize_result


TOKEN = "local-test-token-12345"
STATUS = {"protocol": KIT_PROTOCOL, "configured": True, "connected": True,
          "lcdReady": True, "columns": 21, "rows": 4}
ACK = {"ready": True, "columns": 21, "rows": 4, "dfuCapable": False}


class Response:
    def __init__(self, status=200, value=None, raw=None, *, keep_alive=False):
        self.status = status
        self.body = raw if raw is not None else json.dumps(value).encode()
        self.will_close = not keep_alive
        self.chunked = False

    def getheader(self, name, default=None):
        return {"Connection": "close" if self.will_close else "keep-alive",
                "Content-Length": str(len(self.body))}.get(name, default)

    def read(self, size):
        return self.body[:size]


class Transport:
    """Substitutes HTTPConnection only; request encoding and validation run."""
    def __init__(self, *responses):
        self.responses = list(responses)
        self.calls = []
        self.connections = []

    def connect(self, host, port, timeout):
        transport = self

        class Connection:
            closed = False

            def __init__(self):
                self.sock, self.peer = socket.socketpair()

            def request(self, method, path, body, headers):
                transport.calls.append((host, port, timeout, method, path, body, headers))

            def getresponse(self):
                response = transport.responses.pop(0)
                if isinstance(response, Exception):
                    raise response
                return response

            def close(self):
                self.closed = True
                self.sock.close()
                self.peer.close()

        connection = Connection()
        self.connections.append(connection)
        return connection

    def close(self):
        for connection in self.connections:
            connection.close()


class GatewayTests(unittest.TestCase):
    def setUp(self):
        self.environment = patch.dict(os.environ, {}, clear=True)
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def transport(self, *responses):
        transport = Transport(*responses)
        self.addCleanup(transport.close)
        patched = patch("voice_tactical.gateway.http.client.HTTPConnection", side_effect=transport.connect)
        patched.start()
        self.addCleanup(patched.stop)
        return transport

    def gateway(self, host="127.0.0.1:19080", **kwargs):
        return Gateway(host, TOKEN, **kwargs)

    def test_unconfigured_demo_never_connects(self):
        transport = self.transport()
        gateway = Gateway()
        for result in (gateway.status(), gateway.peers(), gateway.display("DEMO"),
                       gateway.select_peer("01:02:03:04:05:06", "random")):
            self.assertFalse(result["ok"])
            self.assertFalse(result["configured"])
            self.assertFalse(result["available"])
            self.assertFalse(result["acknowledged"])
            self.assertEqual(result["code"], "disabled")
            self.assertFalse(result["capabilities"]["ota"])
        self.assertEqual(transport.calls, [])

    def test_concurrent_callers_do_not_overlap_embedded_http_sessions(self):
        first_entered = threading.Event()
        second_started = threading.Event()
        release = threading.Event()
        lock = threading.Lock()
        active = 0
        peak = 0
        results = []

        class Connection:
            def __init__(self, *args, **kwargs):
                nonlocal active, peak
                with lock:
                    active += 1
                    peak = max(peak, active)

            def request(self, *args):
                pass

            def getresponse(self):
                first_entered.set()
                release.wait(2)
                return Response(value=STATUS)

            def close(self):
                nonlocal active
                with lock:
                    active -= 1

        gateway = self.gateway()

        def second_call():
            second_started.set()
            results.append(gateway.status())

        with patch("voice_tactical.gateway.http.client.HTTPConnection", Connection):
            first = threading.Thread(target=lambda: results.append(gateway.status()))
            second = threading.Thread(target=second_call)
            first.start()
            try:
                self.assertTrue(first_entered.wait(2))
                second.start()
                self.assertTrue(second_started.wait(2))
                # Give an unprotected second request time to enter the transport.
                self.assertFalse(release.wait(0.05))
            finally:
                release.set()
                first.join(2)
                if second.ident is not None:
                    second.join(2)
        self.assertEqual(peak, 1)
        self.assertEqual(active, 0)
        self.assertEqual(len(results), 2)
        self.assertTrue(all(result["ready"] for result in results))

    def test_numeric_lan_hosts_only(self):
        for host in ("127.0.0.1", "10.0.0.4:8081", "172.16.4.5", "192.168.0.7",
                     "::1", "[::1]:8081", "[fd12::1]:8080"):
            with self.subTest(host=host):
                self.assertTrue(self.gateway(host).configured)
        for host in ("example.com", "localhost", "http://192.168.0.7", "8.8.8.8", "0.0.0.0",
                     "169.254.2.3", "224.0.0.1", "192.0.2.1", "100.64.0.1", "2001:db8::1",
                     "[::]:8080", "[::ffff:8.8.8.8]", "[fe80::1%en0]", "127.0.0.1:0",
                     "127.0.0.1:65536", "127.0.0.1:abc", "127.0.0.1/path", "127.0.0.1\n"):
            with self.subTest(host=host), self.assertRaises(ValueError) as error:
                self.gateway(host)
            self.assertNotIn(host, str(error.exception))

    def test_timeout_and_token_arguments_are_validated_without_echo(self):
        for timeout in (0, -1, 31, float("nan"), float("inf"), True, "5"):
            with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                self.gateway(timeout=timeout)
        for token in ("short", "sensitive-token\r\nInjected: value", "한글" * 16, "x" * 129):
            with self.subTest(length=len(token)), self.assertRaises(ValueError) as error:
                Gateway("127.0.0.1", token)
            self.assertNotIn(token, str(error.exception))

    def test_env_token_and_firmware_configuration_syntax(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "local.conf"
            config.write_text('CONFIG_GATEWAY_API_TOKEN="replaced-token-12345"\n'
                              '# CONFIG_GATEWAY_API_TOKEN is not set\n'
                              'CONFIG_GATEWAY_API_TOKEN="local-\\"token\\\\-12345678"\n', encoding="utf-8")
            transport = self.transport(Response(value=STATUS), Response(value=STATUS))
            first = Gateway("127.0.0.1", config_path=config)
            self.assertTrue(first.status()["ok"])
            self.assertEqual(transport.calls[0][-1]["Authorization"], 'Bearer local-"token\\-12345678')
            with patch.dict(os.environ, {"NU87_GATEWAY_TOKEN": TOKEN}):
                second = Gateway("127.0.0.1", config_path=config)
            self.assertTrue(second.status()["ok"])
            self.assertEqual(transport.calls[1][-1]["Authorization"], "Bearer " + TOKEN)
            self.assertNotIn(TOKEN, repr(second))
            config.write_text('CONFIG_GATEWAY_API_TOKEN="secret value"\n')
            with self.assertRaises(ValueError) as error:
                Gateway("127.0.0.1", config_path=config)
            self.assertNotIn("secret value", str(error.exception))
            self.assertNotIn(str(config), str(error.exception))

    def test_status_distinguishes_http_ble_and_display_readiness(self):
        self.transport(Response(value={**STATUS, "connected": False}),
                       Response(value={**STATUS, "lcdReady": False}),
                       Response(value=STATUS))
        gateway = self.gateway()
        disconnected, unsubscribed, ready = gateway.status(), gateway.status(), gateway.status()
        self.assertTrue(disconnected["available"])
        self.assertFalse(disconnected["connected"])
        self.assertFalse(disconnected["ready"])
        self.assertTrue(unsubscribed["connected"])
        self.assertFalse(unsubscribed["ready"])
        self.assertTrue(ready["ready"])
        self.assertFalse(ready["capabilities"]["ota"])

    def test_malformed_status_and_unsupported_firmware_cannot_send(self):
        for state in ({}, {**STATUS, "connected": "true"}, {**STATUS, "protocol": "mzsz-lcd"},
                      {**STATUS, "columns": 20}):
            with self.subTest(state=state):
                transport = self.transport(Response(value=state))
                result = self.gateway().display("TEST")
                self.assertFalse(result["ok"])
                self.assertFalse(result["acknowledged"])
                self.assertEqual(len(transport.calls), 1)

    def test_display_success_is_gateway_ack_not_visual_confirmation(self):
        transport = self.transport(Response(value=STATUS), Response(value=ACK))
        result = self.gateway().display("LOCAL\nSIM: GREET")
        self.assertTrue(result["ok"])
        self.assertTrue(result["acknowledged"])
        self.assertFalse(result["visual_verified"])
        self.assertEqual(result["verification"], "gateway-matching-ack")
        self.assertEqual(transport.calls[1][3:6], ("POST", "/v1/display", b"LOCAL\nSIM: GREET"))
        self.assertTrue(all(connection.closed for connection in transport.connections))

    def test_keep_alive_reuses_complete_authenticated_responses_and_obeys_close(self):
        transport = self.transport(Response(value=STATUS, keep_alive=True),
                                   Response(value=ACK, keep_alive=True),
                                   Response(value=STATUS), Response(value=STATUS))
        gateway = self.gateway()
        with patch("voice_tactical.gateway.time.monotonic", return_value=10):
            self.assertTrue(gateway.display("FIRST")["acknowledged"])
            self.assertTrue(gateway.status()["ready"])
            self.assertEqual(len(transport.connections), 1)
            self.assertTrue(transport.connections[0].closed)
            self.assertTrue(gateway.status()["ready"])
        self.assertEqual(len(transport.connections), 2)
        self.assertTrue(all(call[-1]["Authorization"] == "Bearer " + TOKEN
                            for call in transport.calls))
        self.assertTrue(all(call[-1]["Connection"] == "keep-alive"
                            for call in transport.calls))

    def test_idle_keep_alive_expires_before_firmware_idle_timeout(self):
        transport = self.transport(Response(value=STATUS, keep_alive=True),
                                   Response(value=STATUS))
        gateway = self.gateway()
        with patch("voice_tactical.gateway.time.monotonic", return_value=10):
            self.assertTrue(gateway.status()["ready"])
        with patch("voice_tactical.gateway.time.monotonic", return_value=10.5):
            self.assertTrue(gateway.status()["ready"])
        self.assertEqual(len(transport.connections), 2)
        self.assertTrue(all(connection.closed for connection in transport.connections))

    def test_peer_fin_is_discarded_before_sending_next_display_once(self):
        transport = self.transport(Response(value=STATUS, keep_alive=True),
                                   Response(value=ACK, keep_alive=True),
                                   Response(value=ACK))
        gateway = self.gateway()
        with patch("voice_tactical.gateway.time.monotonic", return_value=10):
            self.assertTrue(gateway.display("FIRST", reuse_status=True)["acknowledged"])
            # Real socket EOF, despite a keep-alive header and unexpired budget.
            transport.connections[0].peer.shutdown(socket.SHUT_WR)
            self.assertTrue(gateway.display("SECOND", reuse_status=True)["acknowledged"])
        self.assertTrue(transport.connections[0].closed)
        self.assertEqual(len(transport.connections), 2)
        self.assertEqual([call[5] for call in transport.calls], [None, b"FIRST", b"SECOND"])

    def test_delayed_reply_does_not_extend_keep_alive_budget(self):
        with patch("voice_tactical.gateway.time.monotonic", return_value=10) as clock:
            class DelayedResponse(Response):
                def read(self, size):
                    clock.return_value = 10.6
                    return super().read(size)

            transport = self.transport(DelayedResponse(value=STATUS, keep_alive=True),
                                       Response(value=STATUS))
            gateway = self.gateway()
            self.assertTrue(gateway.status()["ready"])
            self.assertTrue(transport.connections[0].closed)
            clock.return_value = 10.7
            self.assertTrue(gateway.status()["ready"])
        self.assertEqual(len(transport.connections), 2)
        self.assertEqual(len(transport.calls), 2)

    def test_failed_reused_display_connection_is_closed_without_retrying_command(self):
        transport = self.transport(Response(value=STATUS, keep_alive=True),
                                   OSError("socket closed"), Response(value=STATUS))
        gateway = self.gateway()
        with patch("voice_tactical.gateway.time.monotonic", return_value=10):
            result = gateway.display("SEND ONCE")
            self.assertEqual(result["code"], "unreachable")
            self.assertEqual(len(transport.calls), 2)
            self.assertEqual(len(transport.connections), 1)
            self.assertTrue(transport.connections[0].closed)
            self.assertTrue(gateway.status()["ready"])
        self.assertEqual(len(transport.connections), 2)

    def test_invalid_persistent_json_cannot_leak_a_reusable_socket(self):
        transport = self.transport(Response(raw=b"invalid", keep_alive=True),
                                   Response(value=STATUS))
        gateway = self.gateway()
        self.assertEqual(gateway.status()["code"], "invalid_response")
        self.assertTrue(transport.connections[0].closed)
        self.assertTrue(gateway.status()["ready"])
        self.assertEqual(len(transport.connections), 2)

    def test_live_display_reuses_only_recent_acknowledged_status(self):
        transport = self.transport(Response(value=STATUS), Response(value=ACK),
                                   Response(value=ACK), Response(value=STATUS),
                                   Response(value=ACK))
        gateway = self.gateway()
        with patch("voice_tactical.gateway.time.monotonic", return_value=10):
            self.assertTrue(gateway.display("FIRST", reuse_status=True)["acknowledged"])
        with patch("voice_tactical.gateway.time.monotonic", return_value=11):
            self.assertTrue(gateway.display("SECOND", reuse_status=True)["acknowledged"])
        self.assertEqual([call[4] for call in transport.calls],
                         ["/v1/status", "/v1/display", "/v1/display"])
        with patch("voice_tactical.gateway.time.monotonic", return_value=15):
            self.assertTrue(gateway.display("EXPIRED", reuse_status=True)["acknowledged"])
        self.assertEqual([call[4] for call in transport.calls[-2:]],
                         ["/v1/status", "/v1/display"])

    def test_failed_live_ack_invalidates_cached_display_readiness(self):
        transport = self.transport(Response(value=STATUS), Response(value=ACK),
                                   Response(422, {"error": -128}),
                                   Response(value={**STATUS, "connected": False}))
        gateway = self.gateway()
        with patch("voice_tactical.gateway.time.monotonic", return_value=10):
            self.assertTrue(gateway.display("FIRST", reuse_status=True)["acknowledged"])
            self.assertFalse(gateway.display("FAILED", reuse_status=True)["acknowledged"])
            self.assertEqual(gateway.display("RECHECK", reuse_status=True)["code"], "not_ready")
        self.assertEqual([call[4] for call in transport.calls],
                         ["/v1/status", "/v1/display", "/v1/display", "/v1/status"])

    def test_failed_or_malformed_ack_is_never_success_and_never_retried(self):
        for response in (Response(202, ACK), Response(422, {"error": -110}),
                         Response(value={**ACK, "ready": False}), Response(value={**ACK, "rows": 3}),
                         Response(value={"ready": True}), Response(value=[]),
                         Response(raw=b"not JSON"), Response(raw=b"x" * 4097), TimeoutError(TOKEN)):
            with self.subTest(response=type(response).__name__):
                transport = self.transport(Response(value=STATUS), response)
                result = self.gateway().display("TEST")
                self.assertFalse(result["ok"])
                self.assertFalse(result["acknowledged"])
                self.assertNotIn(TOKEN, json.dumps(result))
                self.assertEqual(len(transport.calls), 2)
                self.assertTrue(all(connection.closed for connection in transport.connections))

    def test_not_ready_stops_before_write(self):
        transport = self.transport(Response(value={**STATUS, "lcdReady": False}))
        result = self.gateway().display("TEST")
        self.assertEqual(result["code"], "not_ready")
        self.assertEqual(len(transport.calls), 1)

    def test_text_fits_firmware_byte_and_row_limits(self):
        for text in ("", "한글", "bad\ttext", "x" * 85, "\n\n\n\nx", "x" * 22 + "\n" + "y" * 43):
            with self.subTest(text=text):
                transport = self.transport()
                result = self.gateway().display(text)
                self.assertEqual(result["code"], "invalid_text")
                self.assertEqual(transport.calls, [])
        transport = self.transport(Response(value=STATUS), Response(value=ACK))
        self.assertTrue(self.gateway().display("x" * 84)["ok"])

    def test_peers_and_explicit_selection_do_not_claim_connected(self):
        peer = {"address": "AA:BB:CC:DD:EE:FF", "type": "random", "name": "NU40-KIT-DISPLAY", "rssi": -55}
        transport = self.transport(Response(value=STATUS), Response(value={"peers": [peer]}),
                                   Response(value=STATUS), Response(202, {"accepted": True}))
        gateway = self.gateway()
        self.assertEqual(gateway.peers()["peers"], [peer])
        result = gateway.select_peer(peer["address"].lower(), peer["type"])
        self.assertTrue(result["accepted"])
        self.assertFalse(result["connected"])
        self.assertTrue(result["connection_pending"])
        self.assertEqual(transport.calls[-1][3:6], ("POST", "/v1/peer", b"AA:BB:CC:DD:EE:FF random"))

    def test_invalid_peer_candidates_and_selection(self):
        transport = self.transport(Response(value=STATUS), Response(value={"peers": [{"address": TOKEN}]}))
        result = self.gateway().peers()
        self.assertFalse(result["ok"])
        self.assertNotIn(TOKEN, json.dumps(result))
        for address, kind in (("CoreBluetooth-UUID", "random"), ("AA:BB:CC:DD:EE:FF", "random\n")):
            result = self.gateway().select_peer(address, kind)
            self.assertEqual(result["code"], "invalid_peer")
        self.assertEqual(len(transport.calls), 2)

    def test_proxy_redirect_and_remote_error_cannot_expose_credentials(self):
        with patch.dict(os.environ, {"HTTP_PROXY": "http://example.com", "ALL_PROXY": "http://example.com"}):
            transport = self.transport(Response(302, {"location": "https://example.com", "error": TOKEN}),
                                       Response(401, {"error": TOKEN}), OSError(TOKEN))
            gateway = self.gateway()
            for _ in range(3):
                result = gateway.status()
                self.assertFalse(result["ok"])
                self.assertNotIn(TOKEN, json.dumps(result))
                self.assertNotIn("example.com", json.dumps(result))
        self.assertEqual(len(transport.calls), 3)
        self.assertTrue(all(call[:2] == ("127.0.0.1", 19080) for call in transport.calls))
        self.assertTrue(all(call[4] == "/v1/status" for call in transport.calls))

    def test_summaries_fit_ascii_display(self):
        for kind in ("query", "report", "orders", "motion", "search", "unknown"):
            for motion in ("stop", "dock", "greet", "forward", "back", "left", "right", "한글" * 100, None):
                text = summarize_result({"kind": kind, "motion": motion})
                self.assertLessEqual(len(text.encode("ascii")), 84)
                self.assertLessEqual(len(text.splitlines()), 4)
                self.assertTrue(all(len(line) <= 21 for line in text.splitlines()))
        self.assertIn("SIM: GREET", summarize_result({"kind": "motion", "motion": "greet"}))

    def test_audio_controls_and_capture_evidence(self):
        status = {"available": True, "running": True, "session": 17, "received": 0,
                  "dropped": 0, "queued": 0, "error": 0, "sampleRate": 16000, "codec": "ima-adpcm"}
        transport = self.transport(Response(202, {"error": 0}), Response(value=status),
                                   Response(value={**status, "received": 2, "queued": 2}),
                                   Response(202, {"error": 0}))
        gateway = self.gateway()
        self.assertTrue(gateway.audio_start(17)["accepted"])
        self.assertFalse(gateway.audio_status()["capture_confirmed"])
        self.assertTrue(gateway.audio_status()["capture_confirmed"])
        self.assertTrue(gateway.audio_stop()["accepted"])
        self.assertEqual(transport.calls[0][4:6], ("/v1/audio/start", b"17"))
        self.assertEqual(transport.calls[-1][4:6], ("/v1/audio/stop", b"stop"))
        for session in (0, -1, 2**32, True, "17"):
            self.assertFalse(gateway.audio_start(session)["ok"])
        self.assertEqual(len(transport.calls), 4)

    def test_audio_chunk_validation_timeout_and_bounds(self):
        frame = b"WA\x01\x00" + (17).to_bytes(4, "little") + bytes(168)
        self.transport(Response(raw=frame * 32), Response(raw=b""), Response(raw=frame[:-1]),
                       Response(raw=frame * 33), Response(raw=b"XX" + frame[2:]), TimeoutError(TOKEN))
        gateway = self.gateway()
        self.assertEqual(gateway.audio_chunk(), frame * 32)
        self.assertEqual(gateway.audio_chunk(), b"")
        for _ in range(4):
            with self.assertRaises(RuntimeError) as error:
                gateway.audio_chunk()
            self.assertNotIn(TOKEN, str(error.exception))

    def test_smp_matches_reply_and_does_not_retry_ambiguous_errors(self):
        packet = bytes((2, 0, 0, 1, 0, 1, 12, 1, 0xa0))
        reply = bytes((3, 0, 0, 1, 0, 1, 12, 1, 0xa0))
        transport = self.transport(Response(raw=reply), Response(raw=reply[:6] + b"\x0d" + reply[7:]),
                                   Response(503, {"error": TOKEN}), Response(value={"available": True}))
        gateway = self.gateway()
        self.assertEqual(gateway.smp(packet), reply)
        for _ in range(2):
            with self.assertRaises(RuntimeError) as error:
                gateway.smp(packet)
            self.assertNotIn(TOKEN, str(error.exception))
        self.assertTrue(gateway.smp_status()["available"])
        self.assertEqual(len(transport.calls), 4)
        with self.assertRaises(ValueError):
            gateway.smp(packet[:-1])
        self.assertEqual(len(transport.calls), 4)

    def test_alarm_output_ack_and_button_event_status(self):
        event = {"available": True, "active": True, "acknowledged": True, "eventCounter": 3,
                 "generation": 1, "source": "button", "outputStatus": 0}
        self.transport(Response(value={"available": True, "active": True, "acknowledged": True}),
                       Response(value={"available": True, "active": False, "acknowledged": False}),
                       Response(value=event))
        gateway = self.gateway()
        self.assertTrue(gateway.alarm(True)["acknowledged"])
        self.assertFalse(gateway.alarm(False)["ok"])
        self.assertEqual(gateway.alarm_status()["eventCounter"], 3)
        self.assertFalse(gateway.alarm("on")["ok"])

    def test_wallaby_command_must_match_acceptance_and_alert_rejects_motion(self):
        accepted = {"accepted": True, "command": "forward", "state": "moving", "sequence": 4,
                    "alert": False, "simulated": True}
        self.transport(Response(value=accepted), Response(422, {"error": -1}),
                       Response(value={**accepted, "command": "stop", "state": "alert", "alert": True}),
                       Response(value={**accepted, "command": "stop"}))
        gateway = self.gateway()
        self.assertTrue(gateway.robot_command("forward")["accepted"])
        self.assertFalse(gateway.robot_command("forward")["accepted"])
        self.assertTrue(gateway.robot_status()["alert"])
        self.assertFalse(gateway.robot_command("forward")["accepted"])
        self.assertFalse(gateway.robot_command("invalid")["ok"])


if __name__ == "__main__":
    unittest.main()
